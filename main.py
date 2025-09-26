import socket
import subprocess
import time
from threading import Thread
import webbrowser
import pyautogui
import uuid
import os

from flask import Flask, request, jsonify
from zeroconf import ServiceInfo, Zeroconf

# --- НАСТРОЙКИ СЕРВЕРА ---
SERVER_NAME = "pc-server"
SERVER_PORT = 8080

# ########################################################################## #
# ##                  НОВЫЙ ПРОФИЛЬ СЕРВЕРА С КАТЕГОРИЯМИ                   ## #
# ##       Теперь команды сгруппированы в словаре "categories"            ## #
# ########################################################################## #
SERVER_PROFILE = {
    "name": "MainPC",  # Понятное имя для отображения на M5Stick
    "categories": {
        # Название категории: [список команд в этой категории]
        
        "media": [
            {"label": "Volume", "command_id": "volume_mode"}
        ],
        "system": [
            {"label": "VS Code", "command_id": "vscode"},
            {"label": "Settings", "command_id": "open_settings"}
        ]
    }
}

app = Flask(__name__)

def get_server_id():
    return ':'.join(['{:02x}'.format((uuid.getnode() >> i) & 0xff) for i in range(0,8*6,8)][::-1])

# --- ОБРАБОТЧИК КОМАНД ---
def execute_command(command_str, url=None):
    """Выполняет действие на ПК в зависимости от полученной команды."""
    print(f"Получена команда: '{command_str}' с URL: '{url}'")
    try:
        if command_str == "launch_steam":
            # Укажите правильный путь к вашему steam.exe
            subprocess.Popen(['C:\\Program Files (x86)\\Steam\\steam.exe'])
            return True
        elif command_str == "launch_discord":
             # Укажите правильный путь к вашему discord.exe
            subprocess.Popen([os.path.expanduser('~\\AppData\\Local\\Discord\\Update.exe'), '--processStart', 'Discord.exe'])
            return True
        elif command_str == "media_play_pause":
            pyautogui.press('playpause'); return True
        elif command_str == "media_next":
            pyautogui.press('nexttrack'); return True
        elif command_str == "vscode":
            os.system('code'); return True
        elif command_str == "open_settings":
            if url: webbrowser.open(url); return True
            else: return False
        elif command_str == "vol_up":
            pyautogui.press('volumeup'); return True
        elif command_str == "vol_down":
            pyautogui.press('volumedown'); return True
        else:
            print(f"Неизвестная команда: '{command_str}'"); return False
    except Exception as e:
        print(f"ERR: Ошибка выполнения: {e}"); return False

# --- ВЕБ-СЕРВЕР (FLASK) ---
@app.route('/info', methods=['GET'])
def get_info():
    response = {"id": get_server_id(), "profile": SERVER_PROFILE}
    return jsonify(response)

@app.route('/execute', methods=['GET'])
def run_script():
    command = request.args.get('command')
    url_to_open = request.args.get('url')
    if not command: return jsonify({"status": "error", "message": "Параметр 'command' не указан"}), 400
    if execute_command(command, url_to_open): return jsonify({"status": "success"})
    else: return jsonify({"status": "error"}), 404

# --- MDNS и ЗАПУСК (без изменений) ---
def register_mdns_service():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("8.8.8.8", 80))
        ip_address = s.getsockname()[0]; s.close()
    except Exception: ip_address = "127.0.0.1"
    service_info = ServiceInfo("_http._tcp.local.", f"{SERVER_NAME}._http._tcp.local.", addresses=[socket.inet_aton(ip_address)], port=SERVER_PORT, properties={'path': '/'}, server=f"{SERVER_NAME}.local.")
    zeroconf = Zeroconf()
    print(f"Регистрация mDNS сервиса: http://{SERVER_NAME}.local:{SERVER_PORT}"); print(f"ID Сервера: {get_server_id()}"); print(f"Сервер слушает на IP: http://{ip_address}:{SERVER_PORT}")
    zeroconf.register_service(service_info)
    try:
        while True: time.sleep(1)
    finally: zeroconf.unregister_service(service_info); zeroconf.close()

if __name__ == '__main__':
    print("------ Сервер запуска скриптов для M5Stick ------")
    mdns_thread = Thread(target=register_mdns_service, daemon=True); mdns_thread.start()
    app.run(host='0.0.0.0', port=8080)