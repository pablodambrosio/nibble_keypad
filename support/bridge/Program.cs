using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

class Program
{
    private const string SVC_UUID_PREFIX = "0000fff0";
    private const string CHR_PWR_PREFIX = "0000fff1";
    private const string CHR_KEY_PREFIX = "0000fff2";
    private const string CHR_RST_PREFIX = "0000fff3";

    private static BluetoothLEDevice? s_device;
    private static GattCharacteristic? s_chrPower;
    private static GattCharacteristic? s_chrKeys;
    private static GattCharacteristic? s_chrReset;
    private static bool s_isConnecting = false;

    static async Task Main(string[] args)
    {
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        EmitJson(new { @event = "bridge_started", message = "Nibble BLE WinRT bridge initialized" });

        // Initial connection attempt
        _ = Task.Run(async () =>
        {
            while (true)
            {
                if (s_device == null)
                {
                    await TryInitDeviceAsync();
                }
                else if (s_device.ConnectionStatus == BluetoothConnectionStatus.Connected && (s_chrPower == null || s_chrKeys == null))
                {
                    await DiscoverCharacteristicsAsync();
                }
                await Task.Delay(4000);
            }
        });

        // Read commands from stdin line-by-line
        using var reader = new StreamReader(Console.OpenStandardInput());
        string? line;
        while ((line = await reader.ReadLineAsync()) != null)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            try
            {
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                if (!root.TryGetProperty("cmd", out var cmdProp)) continue;
                string cmd = cmdProp.GetString() ?? "";

                switch (cmd)
                {
                    case "status":
                        await SendStatusAsync();
                        break;

                    case "read":
                        await ReadConfigAsync();
                        break;

                    case "write_power":
                        if (root.TryGetProperty("value", out var pwrVal))
                        {
                            byte pwr = (byte)pwrVal.GetInt32();
                            await WritePowerAsync(pwr);
                        }
                        break;

                    case "write_keys":
                        if (root.TryGetProperty("keysHex", out var keysHexProp))
                        {
                            string hex = keysHexProp.GetString() ?? "";
                            await WriteKeysAsync(hex);
                        }
                        break;

                    case "reset":
                        await WriteResetAsync();
                        break;

                    default:
                        EmitJson(new { @event = "error", message = $"Unknown command '{cmd}'" });
                        break;
                }
            }
            catch (Exception ex)
            {
                EmitJson(new { @event = "error", message = ex.Message });
            }
        }
    }

    private static async Task TryInitDeviceAsync()
    {
        if (s_isConnecting) return;
        s_isConnecting = true;

        try
        {
            string selector = BluetoothLEDevice.GetDeviceSelectorFromPairingState(true);
            var devices = await DeviceInformation.FindAllAsync(selector);
            DeviceInformation? target = devices.FirstOrDefault(d => d.Name.Contains("Nibble", StringComparison.OrdinalIgnoreCase));

            if (target == null)
            {
                EmitJson(new { @event = "device_status", connected = false, message = "Nibble Keypad not found in Windows paired devices." });
                return;
            }

            s_device?.Dispose();
            s_device = await BluetoothLEDevice.FromIdAsync(target.Id);
            if (s_device == null)
            {
                EmitJson(new { @event = "device_status", connected = false, message = "Could not open Bluetooth device." });
                return;
            }

            s_device.ConnectionStatusChanged += (dev, args) =>
            {
                bool connected = dev.ConnectionStatus == BluetoothConnectionStatus.Connected;
                EmitJson(new { @event = "connection_changed", connected });
                if (connected)
                {
                    _ = Task.Run(async () =>
                    {
                        await Task.Delay(600);
                        await DiscoverCharacteristicsAsync();
                        await ReadConfigAsync();
                    });
                }
            };

            bool isConnected = s_device.ConnectionStatus == BluetoothConnectionStatus.Connected;
            EmitJson(new
            {
                @event = "device_init",
                name = s_device.Name,
                connected = isConnected,
                message = isConnected ? "Device active" : "Device is sleeping. Press any key to wake."
            });

            if (isConnected)
            {
                await DiscoverCharacteristicsAsync();
                await ReadConfigAsync();
            }
        }
        catch (Exception ex)
        {
            EmitJson(new { @event = "error", message = $"TryInitDeviceAsync: {ex.Message}" });
        }
        finally
        {
            s_isConnecting = false;
        }
    }

    private static async Task DiscoverCharacteristicsAsync()
    {
        try
        {
            if (s_device == null) return;

            var servicesResult = await s_device.GetGattServicesAsync(BluetoothCacheMode.Uncached);
            if (servicesResult.Status != GattCommunicationStatus.Success)
            {
                // Fall back to cached if radio is momentarily unresponsive
                servicesResult = await s_device.GetGattServicesAsync(BluetoothCacheMode.Cached);
            }

            if (servicesResult.Status != GattCommunicationStatus.Success)
            {
                EmitJson(new { @event = "device_status", connected = false, message = $"GattServices query: {servicesResult.Status}" });
                return;
            }

            GattDeviceService? cfgSvc = servicesResult.Services.FirstOrDefault(s => s.Uuid.ToString().StartsWith(SVC_UUID_PREFIX, StringComparison.OrdinalIgnoreCase));
            if (cfgSvc == null)
            {
                EmitJson(new { @event = "device_status", connected = false, message = "Config service (0xFFF0) not found" });
                return;
            }

            var access = await cfgSvc.RequestAccessAsync();
            if (access != DeviceAccessStatus.Allowed)
            {
                EmitJson(new { @event = "device_status", connected = false, message = $"Access to 0xFFF0 denied: {access}" });
                return;
            }



            var charsResult = await cfgSvc.GetCharacteristicsAsync(BluetoothCacheMode.Uncached);
            if (charsResult.Status != GattCommunicationStatus.Success)
            {
                charsResult = await cfgSvc.GetCharacteristicsAsync(BluetoothCacheMode.Cached);
            }

            if (charsResult.Status != GattCommunicationStatus.Success)
            {
                EmitJson(new { @event = "device_status", connected = false, message = $"Characteristics query: {charsResult.Status}" });
                return;
            }

            foreach (var ch in charsResult.Characteristics)
            {
                string uuid = ch.Uuid.ToString();
                if (uuid.StartsWith(CHR_PWR_PREFIX, StringComparison.OrdinalIgnoreCase)) s_chrPower = ch;
                else if (uuid.StartsWith(CHR_KEY_PREFIX, StringComparison.OrdinalIgnoreCase)) s_chrKeys = ch;
                else if (uuid.StartsWith(CHR_RST_PREFIX, StringComparison.OrdinalIgnoreCase)) s_chrReset = ch;
            }

            EmitJson(new
            {
                @event = "device_ready",
                name = s_device.Name,
                connected = s_device.ConnectionStatus == BluetoothConnectionStatus.Connected,
                hasPowerChar = s_chrPower != null,
                hasKeysChar = s_chrKeys != null,
                hasResetChar = s_chrReset != null
            });
        }
        catch (Exception ex)
        {
            EmitJson(new { @event = "error", message = $"DiscoverCharacteristicsAsync: {ex.Message}" });
        }
    }

    private static async Task SendStatusAsync()
    {
        bool connected = s_device != null && s_device.ConnectionStatus == BluetoothConnectionStatus.Connected;
        EmitJson(new
        {
            @event = "status",
            connected,
            deviceName = s_device?.Name ?? "Nibble Keypad"
        });
    }

    private static async Task ReadConfigAsync()
    {
        try
        {
            if (s_chrPower == null || s_chrKeys == null)
            {
                await DiscoverCharacteristicsAsync();
            }

            if (s_chrPower == null || s_chrKeys == null)
            {
                EmitJson(new { @event = "device_status", connected = false, message = "Characteristics not ready. Wake keypad." });
                return;
            }

            var pwrRes = await s_chrPower.ReadValueAsync(BluetoothCacheMode.Uncached);
            var keyRes = await s_chrKeys.ReadValueAsync(BluetoothCacheMode.Uncached);

            if (pwrRes.Status == GattCommunicationStatus.Success && keyRes.Status == GattCommunicationStatus.Success)
            {
                byte pwr = pwrRes.Value.GetByte(0);
                byte[] keyBytes = keyRes.Value.ToArray();
                string hex = Convert.ToHexString(keyBytes).ToLowerInvariant();

                EmitJson(new
                {
                    @event = "config",
                    power = (int)pwr,
                    keysHex = hex
                });
            }
            else
            {
                EmitJson(new { @event = "device_status", connected = false, message = $"ReadConfig: pwr={pwrRes.Status}, keys={keyRes.Status}" });
            }
        }
        catch (Exception ex)
        {
            EmitJson(new { @event = "error", message = $"ReadConfigAsync exception: {ex.Message}" });
        }
    }

    private static async Task WritePowerAsync(byte pwr)
    {
        if (s_chrPower == null)
        {
            EmitJson(new { @event = "write_result", characteristic = "power", success = false, message = "Power characteristic not ready. Wake keypad." });
            return;
        }

        var writer = new DataWriter();
        writer.WriteByte(pwr);
        var res = await s_chrPower.WriteValueAsync(writer.DetachBuffer());
        EmitJson(new
        {
            @event = "write_result",
            characteristic = "power",
            success = res == GattCommunicationStatus.Success,
            status = res.ToString()
        });
    }

    private static async Task WriteKeysAsync(string hex)
    {
        if (s_chrKeys == null)
        {
            EmitJson(new { @event = "write_result", characteristic = "keys", success = false, message = "Key mapping characteristic not ready. Wake keypad." });
            return;
        }

        byte[] bytes = Convert.FromHexString(hex);
        var writer = new DataWriter();
        writer.WriteBytes(bytes);
        var res = await s_chrKeys.WriteValueAsync(writer.DetachBuffer());
        EmitJson(new
        {
            @event = "write_result",
            characteristic = "keys",
            success = res == GattCommunicationStatus.Success,
            status = res.ToString()
        });
    }

    private static async Task WriteResetAsync()
    {
        if (s_chrReset == null)
        {
            EmitJson(new { @event = "write_result", characteristic = "reset", success = false, message = "Reset characteristic not ready. Wake keypad." });
            return;
        }

        var writer = new DataWriter();
        writer.WriteByte(0x01);
        var res = await s_chrReset.WriteValueAsync(writer.DetachBuffer());
        EmitJson(new
        {
            @event = "write_result",
            characteristic = "reset",
            success = res == GattCommunicationStatus.Success,
            status = res.ToString()
        });
    }

    private static void EmitJson(object payload)
    {
        string json = JsonSerializer.Serialize(payload);
        Console.WriteLine(json);
    }
}
