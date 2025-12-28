Import("env")
import re
import os

def get_ota_password():
    """
    Extrahiert OTA_PASSWORD aus credentials.h
    """
    # Suche credentials.h in den include paths
    credentials_paths = [
        "/home/robothaler/credentials/credentials.h",
    ]
    
    for path in credentials_paths:
        if os.path.exists(path):
            try:
                with open(path, 'r') as f:
                    content = f.read()
                    
                # Suche nach #define OTA_PASSWORD "..."
                patterns = [
                    r'#define\s+OTA_PASSWORD\s+"([^"]+)"',
                    r'#define\s+OTA_PASSWORD\s+\'([^\']+)\'',
                ]
                
                for pattern in patterns:
                    match = re.search(pattern, content)
                    if match:
                        password = match.group(1)
                        print(f"✓ OTA Password erfolgreich aus {path} geladen")
                        return password
                        
            except Exception as e:
                print(f"⚠ Fehler beim Lesen von {path}: {e}")
                continue
    
    # Fallback: Warnung ausgeben
    print("⚠ OTA_PASSWORD nicht in credentials.h gefunden!")
    print("⚠ Verwende Fallback-Passwort 'admin' - ÄNDERE DAS!")
    return "admin"

# Extrahiere Passwort
ota_password = get_ota_password()

# Injiziere es in die Upload-Flags
current_flags = env.get("UPLOADERFLAGS", [])

# Entferne altes --auth Flag falls vorhanden
current_flags = [f for f in current_flags if not f.startswith("--auth=")]

# Füge neues --auth Flag hinzu
current_flags.append(f"--auth={ota_password}")

env.Replace(UPLOADERFLAGS=current_flags)

print(f"✓ OTA Upload-Flags konfiguriert")
