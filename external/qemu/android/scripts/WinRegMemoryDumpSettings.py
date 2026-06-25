import winreg

def read_registry_key(hive, sub_key, value_name):
    """Reads a value from the specified registry key."""
    try:
        with winreg.OpenKey(hive, sub_key) as key:
            value, regtype = winreg.QueryValueEx(key, value_name)
            return value
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"Error reading registry: {e}")
        return None

def write_registry_key(hive, sub_key, value_name, value, reg_type):
     """Writes a value to the specified registry key."""
     try:
        with winreg.CreateKey(hive, sub_key) as key:
            winreg.SetValueEx(key, value_name, 0, reg_type, value)
            return True
     except Exception as e:
        print(f"Error writing to registry: {e}")
        return False

# Example Usage
hive = winreg.HKEY_LOCAL_MACHINE
sub_key = r"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\qemu-system-x86_64.exe" 

# How many dumps files can be stored
value_name = "DumpCount"
new_value = 15
value = read_registry_key(hive, sub_key, value_name)
if value:
    print(f"The value of {value_name} is: {value}")
if write_registry_key(hive, sub_key, value_name, new_value, winreg.REG_DWORD):
    print(f"Successfully wrote {new_value} to {value_name}")

# DumpType:
#          1     Minidump
#          2     Full Dump
value_name = "DumpType"
new_value = 2
value = read_registry_key(hive, sub_key, value_name)
if value:
    print(f"The value of {value_name} is: {value}")
if write_registry_key(hive, sub_key, value_name, new_value, winreg.REG_DWORD):
    print(f"Successfully wrote {new_value} to {value_name}")

# Location of the dump files
#     %LOCALAPPDATA% means C:\Users\XXXXX\AppData\Local
value_name = "DumpFolder"
new_value = "%LOCALAPPDATA%\\Google\\AndroidEmulator\\CrashDumps"
value = read_registry_key(hive, sub_key, value_name)
if value:
    print(f"The value of {value_name} is: {value}")
if write_registry_key(hive, sub_key, value_name, new_value, winreg.REG_EXPAND_SZ):
    print(f"Successfully wrote {new_value} to {value_name}")
