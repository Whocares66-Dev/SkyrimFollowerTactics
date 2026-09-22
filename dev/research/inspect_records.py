"""Read-only houseCARL research; writes evidence only beneath this directory."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
REQUESTS = [
    ('status', 'housecarl_load_order_status', {}),
    ('lydia', 'housecarl_read_record', {'formid': '0A2C8E:Skyrim.esm', 'depth': 3, 'resolve_names': True, 'conflict_tree': True}),
    ('marcurio', 'housecarl_read_record', {'formid': '0B9980:Skyrim.esm', 'depth': 3, 'resolve_names': True, 'fields': ['Configuration', 'Class', 'ActorEffect', 'Perks', 'Template', 'CombatStyle']}),
    ('flames-tome', 'housecarl_read_record', {'formid': '09CD51:Skyrim.esm', 'depth': 3, 'resolve_names': True}),
    ('warrior-class', 'housecarl_read_record', {'formid': '013176:Skyrim.esm', 'depth': 3}),
    ('flames', 'housecarl_read_record', {'formid': '012FCD:Skyrim.esm', 'depth': 3, 'resolve_names': True}),
    ('armsman', 'housecarl_read_record', {'formid': '0BABE4:Skyrim.esm', 'depth': 4, 'resolve_names': True}),
]

def main():
    env = dict(os.environ, HouseCarl__Mo2InstanceDir=r'C:\modding\MO2',
               HOUSECARL_DATA_DIR=str(ROOT / 'housecarl-cache'))
    proc = subprocess.Popen([r'C:\modding\houseCARL\housecarl\server\housecarl-mcp.exe'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding='utf-8', env=env)
    def call(i, method, params):
        proc.stdin.write(json.dumps(dict(jsonrpc='2.0', id=i, method=method, params=params)) + '\n')
        proc.stdin.flush()
        while True:
            line = proc.stdout.readline()
            if not line:
                raise RuntimeError('houseCARL exited')
            try:
                result = json.loads(line)
            except ValueError:
                continue
            if result.get('id') == i:
                return result
    try:
        call(1, 'initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                              'clientInfo': {'name': 'progression-research', 'version': '1'}})
        proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
        proc.stdin.flush()
        for i, (label, name, args) in enumerate(REQUESTS, 2):
            result = call(i, 'tools/call', {'name': name, 'arguments': args})
            (ROOT / (label + '.json')).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
            print(label, json.dumps(result, ensure_ascii=True), flush=True)
    finally:
        proc.terminate()
        proc.wait(timeout=10)

if __name__ == '__main__':
    main()
