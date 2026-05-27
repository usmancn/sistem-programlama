import os
import subprocess
import time
from flask import Flask, render_template, request, jsonify

app = Flask(__name__)

# Path to the C project files
BASE_DIR = os.path.dirname(os.path.dirname(__file__))
CLIENT_BIN = os.path.join(BASE_DIR, 'client')
LOG_FILE = os.path.join(BASE_DIR, 'server.log')

# Server start time for uptime tracking
SERVER_START_TIME = time.time()

# Real operation counters (incremented on every actual C client call)
stats = {
    "set": 0,
    "get": 0,
    "delete": 0,
    "list": 0,
    "errors": 0
}

def run_client(args):
    """Executes the C client binary and returns the output."""
    try:
        result = subprocess.run(
            [CLIENT_BIN] + args,
            capture_output=True,
            text=True,
            check=True
        )
        return {"status": "success", "message": result.stdout.strip()}
    except subprocess.CalledProcessError as e:
        stats["errors"] += 1
        return {
            "status": "error",
            "message": e.stdout.strip() if e.stdout else "Connection failed. Is the server running?"
        }
    except FileNotFoundError:
        stats["errors"] += 1
        return {"status": "error", "message": "Client binary not found. Please run 'make' first."}

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/list', methods=['GET'])
def list_keys():
    stats["list"] += 1
    res = run_client(['LIST'])
    if res['status'] == 'success':
        lines = res['message'].split('\n')
        data = []
        for line in lines:
            line = line.strip()
            if line.startswith('[') and line.endswith(']'):
                content = line[1:-1]
                parts = content.split(':', 1)
                if len(parts) == 2:
                    data.append({
                        "key": parts[0].strip(),
                        "value": parts[1].strip()
                    })
        return jsonify({"status": "success", "data": data})
    return jsonify(res)

@app.route('/api/set', methods=['POST'])
def set_key():
    key = request.form.get('key')
    value = request.form.get('value')
    if not key or not value:
        return jsonify({"status": "error", "message": "Key and Value are required."})
    stats["set"] += 1
    res = run_client(['SET', key, value])
    return jsonify(res)

@app.route('/api/get', methods=['GET'])
def get_key():
    key = request.args.get('key')
    if not key:
        return jsonify({"status": "error", "message": "Key is required."})
    stats["get"] += 1
    res = run_client(['GET', key])
    return jsonify(res)

@app.route('/api/delete', methods=['POST'])
def delete_key():
    key = request.form.get('key')
    if not key:
        return jsonify({"status": "error", "message": "Key is required."})
    stats["delete"] += 1
    res = run_client(['DELETE', key])
    return jsonify(res)

@app.route('/api/logs', methods=['GET'])
def get_logs():
    """Reads the real C server log file."""
    try:
        with open(LOG_FILE, 'r') as f:
            lines = f.readlines()
            last_lines = lines[-40:] if len(lines) > 40 else lines
            return jsonify({"status": "success", "data": last_lines})
    except FileNotFoundError:
        return jsonify({"status": "success", "data": []})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)})

@app.route('/api/stats', methods=['GET'])
def get_stats():
    """Returns real operation statistics and uptime."""
    uptime_seconds = int(time.time() - SERVER_START_TIME)
    hours   = uptime_seconds // 3600
    minutes = (uptime_seconds % 3600) // 60
    seconds = uptime_seconds % 60
    return jsonify({
        "status": "success",
        "data": {
            "set":    stats["set"],
            "get":    stats["get"],
            "delete": stats["delete"],
            "list":   stats["list"],
            "errors": stats["errors"],
            "total":  stats["set"] + stats["get"] + stats["delete"] + stats["list"],
            "uptime": f"{hours:02d}:{minutes:02d}:{seconds:02d}"
        }
    })

if __name__ == '__main__':
    app.run(debug=True, port=8080)
