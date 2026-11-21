#!/usr/bin/env python3
"""The HTTP reply table of a game's own online service for the in-host online services (dm/host/online/online_http.c, DM_ONLINE_HTTP): one row a request,
`<type>\\t<cookie or ->\\t<reply body>`, the type being the request's JSON type (`Request<X>Params`), a path (`/redirect`) or a host-name prefix (`@phx-ping`).

Made from the response schemas of playDB-CNT (github.com/sikoGD/playDB-CNT, tools/dusk_server/response_schemas.json, by sikoGD: the reply types of The Duskbloods
Network Test's /frontend/ API and their fields, read out of the game), with the replies its stand-in server gives for play alone (frontend_api.py, frontdoor.py):
every reply is its type's fields at their empty values (0, false, "", [], nested objects the same), and
  · CreateSession names one local player (ID 1001) and sets the session cookie,
  · GetPingServerUrls lists one ping server for each of the regions 0 to 3,
  · /redirect answers "nochange" (the client keeps its base URL), and any request to a phx-ping host gets {}.
That repository states no licence for its own files: the schema file stays outside this repository and is read from where the user has it. The table made from
it is the profile's dm/games/0400D1D02399A000/online_http.tsv.

Usage: online_http_table.py <response_schemas.json> <out.tsv>   (exit 1 on error)"""
import base64, json, sys

PLAYER = 1001
PING = 'https://phx-ping-nt.fromsoftware-game.net'


def default(spec):
    if isinstance(spec, dict):
        if len(spec) == 1 and next(iter(spec)).startswith('object@'): spec = next(iter(spec.values()))
        return {k: default(v) for k, v in spec.items()}
    if isinstance(spec, list): return []
    return {'int': 0, 'bool': False, 'string': ''}.get(spec, 0)


def session():
    doc = base64.b64encode(json.dumps({'player_id': PLAYER}).encode()).decode()
    return {'player_id': PLAYER, 'player_id_with_signature': {'player_id_document': doc, 'signature_base64': base64.b64encode(bytes(64)).decode()},
            'display_user_id': f'LOCAL-{PLAYER}', 'user_id': f'local-{PLAYER}', 'client_ip': '127.0.0.1', 'redirect_addr': '', 'required_relogin_sec': 86400,
            'environment_name': 'local', 'show_environment_name_in_lobby': False}


def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    try: schemas = json.load(open(sys.argv[1], encoding='utf-8'))
    except (OSError, ValueError) as e: sys.exit(f'{sys.argv[1]}: {e}')
    over = {'ResponseCreateSessionParams': session(),
            'ResponseGetPingServerUrlsParams': {'ping_servers': [{'matching_region_id': r, 'ping_server_url': PING} for r in range(4)]}}
    rows = []
    for name, schema in schemas.items():
        if not name.startswith('Response'): sys.exit(f'{sys.argv[1]}: {name} is not a response type')
        value = default(schema.get('fields') or {}); value.update(over.get(name, {}))
        cookie = f'session-id=local{PLAYER}; Path=/' if name == 'ResponseCreateSessionParams' else '-'
        rows.append(('Request' + name[len('Response'):], cookie, json.dumps({'type': name, 'value': value})))
    rows.append(('/redirect', '-', json.dumps({'response': {'redirect': 'nochange'}})))
    rows.append(('@phx-ping', '-', '{}'))
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write('# request type, path or @host prefix\tcookie (- = none)\treply body   (dm/tools/online_http_table.py)\n')
        for r in rows:
            if any('\t' in c or '\n' in c for c in r): sys.exit(f'{r[0]}: a tab or line break in the row')
            f.write('\t'.join(r) + '\n')
    print(f'{sys.argv[2]}: {len(rows)} rows ({len(schemas)} reply types)')


if __name__ == '__main__':
    main()
