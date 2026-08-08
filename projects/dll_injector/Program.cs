using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

internal static class Native
{
    public const uint ProcessCreateThread = 0x0002;
    public const uint ProcessQueryInformation = 0x0400;
    public const uint ProcessVmOperation = 0x0008;
    public const uint ProcessVmWrite = 0x0020;
    public const uint ProcessVmRead = 0x0010;
    public const uint MemCommit = 0x1000;
    public const uint MemRelease = 0x8000;
    public const uint PageReadWrite = 0x04;
    public const uint Infinite = 0xFFFFFFFF;

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint VirtualAllocEx(nint process, nint address, nuint size, uint allocationType, uint protection);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteProcessMemory(nint process, nint address, byte[] buffer, nuint size, out nuint written);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint GetProcAddress(nint module, string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern nint GetModuleHandle(string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern nint LoadLibraryEx(string fileName, nint file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(nint module);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint CreateRemoteThread(nint process, nint attributes, nuint stackSize, nint startAddress, nint parameter, uint flags, out uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(nint handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetExitCodeThread(nint thread, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool VirtualFreeEx(nint process, nint address, nuint size, uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(nint handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsWow64Process2(nint process, out ushort processMachine, out ushort nativeMachine);
}

internal static class Program
{
    private const uint DontResolveDllReferences = 0x00000001;
    private const uint WaitObject0 = 0;
    private const uint WaitTimeout = 0x00000102;

    private static int Main(string[] args)
    {
        if (args.Length > 0 && string.Equals(args[0], "uninject", StringComparison.OrdinalIgnoreCase))
            return Uninject(args);

        if (args.Length > 0 && string.Equals(args[0], "inject", StringComparison.OrdinalIgnoreCase))
            args = args[1..];

        return Inject(args);
    }

    private static int Inject(string[] args)
    {
        if (args.Length is < 2 or > 3 || !uint.TryParse(args[0], out var processId))
        {
            Console.Error.WriteLine("Usage: DllInjector.exe inject <pid> <absolute-dll-path> [timeout-ms]");
            Console.Error.WriteLine("       DllInjector.exe uninject <pid> <module-name-or-path> [timeout-ms]");
            return 2;
        }

        var dllPath = Path.GetFullPath(args[1]);
        var timeout = args.Length == 3 && uint.TryParse(args[2], out var parsedTimeout) ? parsedTimeout : 15_000u;
        if (!File.Exists(dllPath))
        {
            Console.Error.WriteLine($"DLL does not exist: {dllPath}");
            return 3;
        }

        try
        {
            using var process = Process.GetProcessById((int)processId);
            Console.WriteLine($"Target: {process.ProcessName} ({process.Id})");
            if (!IsCompatibleArchitecture(process.Handle))
                return 4;

            var access = Native.ProcessCreateThread | Native.ProcessQueryInformation | Native.ProcessVmOperation |
                         Native.ProcessVmWrite | Native.ProcessVmRead;
            var target = OpenTarget(processId, access);

            try
            {
                var pathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
                var remotePath = Native.VirtualAllocEx(target, 0, (nuint)pathBytes.Length, Native.MemCommit, Native.PageReadWrite);
                if (remotePath == 0)
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "VirtualAllocEx failed");

                try
                {
                    if (!Native.WriteProcessMemory(target, remotePath, pathBytes, (nuint)pathBytes.Length, out var written) || written != (nuint)pathBytes.Length)
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory failed");

                    var kernel32 = Native.GetModuleHandle("kernel32.dll");
                    var loadLibrary = Native.GetProcAddress(kernel32, "LoadLibraryW");
                    if (loadLibrary == 0)
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "GetProcAddress(LoadLibraryW) failed");

                    var thread = Native.CreateRemoteThread(target, 0, 0, loadLibrary, remotePath, 0, out var threadId);
                    if (thread == 0)
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateRemoteThread failed");

                    try
                    {
                        var waitResult = Native.WaitForSingleObject(thread, timeout);
                        if (waitResult != WaitObject0)
                            throw new InvalidOperationException($"Remote loader thread did not finish (wait result 0x{waitResult:X8})");
                        if (!Native.GetExitCodeThread(thread, out var moduleHandle) || moduleHandle == 0)
                            throw new Win32Exception(Marshal.GetLastWin32Error(), "Remote LoadLibraryW returned failure");
                        Console.WriteLine($"Injected successfully; loader thread {threadId}, module 0x{moduleHandle:X8}");
                        return 0;
                    }
                    finally
                    {
                        Native.CloseHandle(thread);
                    }
                }
                finally
                {
                    Native.VirtualFreeEx(target, remotePath, 0, Native.MemRelease);
                }
            }
            finally
            {
                Native.CloseHandle(target);
            }
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    private static int Uninject(string[] args)
    {
        if (args.Length is < 3 or > 4 || !uint.TryParse(args[1], out var processId))
        {
            Console.Error.WriteLine("Usage: DllInjector.exe uninject <pid> <module-name-or-path> [timeout-ms]");
            return 2;
        }

        var moduleSpec = args[2];
        var timeout = args.Length == 4 && uint.TryParse(args[3], out var parsedTimeout) ? parsedTimeout : 15_000u;
        try
        {
            using var process = Process.GetProcessById((int)processId);
            Console.WriteLine($"Target: {process.ProcessName} ({process.Id})");
            if (!IsCompatibleArchitecture(process.Handle))
                return 4;

            var module = FindModule(process, moduleSpec);
            if (module is null)
                throw new InvalidOperationException($"Module not found in target: {moduleSpec}");

            var modulePath = module.FileName;
            var localModule = Native.LoadLibraryEx(modulePath, 0, DontResolveDllReferences);
            if (localModule == 0)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "LoadLibraryEx for export lookup failed");

            try
            {
                var localUnload = Native.GetProcAddress(localModule, "LuaManagerOverlay_Unload");
                if (localUnload == 0)
                    throw new InvalidOperationException("Module does not export LuaManagerOverlay_Unload");

                var rva = localUnload.ToInt64() - localModule.ToInt64();
                if (rva <= 0)
                    throw new InvalidOperationException("Invalid unload export RVA");
                var remoteUnload = new nint(module.BaseAddress.ToInt64() + rva);
                var target = OpenTarget(processId, Native.ProcessCreateThread | Native.ProcessQueryInformation | Native.ProcessVmOperation);
                try
                {
                    var thread = Native.CreateRemoteThread(target, 0, 0, remoteUnload, module.BaseAddress, 0, out var threadId);
                    if (thread == 0)
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateRemoteThread(unload) failed");
                    try
                    {
                        var waitResult = Native.WaitForSingleObject(thread, timeout);
                        if (waitResult == WaitTimeout)
                            throw new InvalidOperationException("Clean unload timed out");
                        if (waitResult != WaitObject0)
                            throw new InvalidOperationException($"Remote unload wait failed (0x{waitResult:X8})");
                        Console.WriteLine($"Clean unload completed; thread {threadId}");
                        return 0;
                    }
                    finally
                    {
                        Native.CloseHandle(thread);
                    }
                }
                finally
                {
                    Native.CloseHandle(target);
                }
            }
            finally
            {
                Native.FreeLibrary(localModule);
            }
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    private static nint OpenTarget(uint processId, uint access)
    {
        var target = Native.OpenProcess(access, false, processId);
        if (target == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");
        return target;
    }

    private static ProcessModule? FindModule(Process process, string moduleSpec)
    {
        var wantedPath = File.Exists(moduleSpec) ? Path.GetFullPath(moduleSpec) : null;
        foreach (ProcessModule module in process.Modules)
        {
            if (wantedPath is not null && string.Equals(Path.GetFullPath(module.FileName), wantedPath, StringComparison.OrdinalIgnoreCase))
                return module;
            if (string.Equals(module.ModuleName, moduleSpec, StringComparison.OrdinalIgnoreCase))
                return module;
        }
        return null;
    }

    private static bool IsCompatibleArchitecture(nint process)
    {
        if (!Native.IsWow64Process2(process, out var processMachine, out var nativeMachine))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "IsWow64Process2 failed");
        var currentMachine = Environment.Is64BitProcess ? (ushort)0x8664 : (ushort)0x014C;
        var targetMachine = processMachine == 0 ? nativeMachine : processMachine;
        if (targetMachine != currentMachine)
        {
            Console.Error.WriteLine($"Architecture mismatch: injector 0x{currentMachine:X4}, target 0x{targetMachine:X4}");
            return false;
        }
        return true;
    }
}
