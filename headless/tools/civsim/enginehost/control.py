"""Client for civhost --control stdio (JSON-lines).

    from civsim.enginehost.control import CivHost
    with CivHost(out_dir) as h:
        h.call('hello')
        h.call('new_game', scenario='headless/scenarios/smoke/pangaea6.json')
        reply, events = h.run(turns=10)

Smoke check (CLI and control channel must give byte-identical digest.bin):
    cd headless/tools; python -m civsim.enginehost.control [--turns 3] [--scenario <json>]
"""
import json
import os
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
EXE = os.path.join(REPO, 'headless', 'out', 'host', 'Release', 'civhost.exe')


class CivHostError(RuntimeError):
    pass


class CivHost(object):
    def __init__(self, out_dir, exe=EXE, extra_args=None):
        args = [exe, '--control', 'stdio', '--out', out_dir] + list(extra_args or [])
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=REPO)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, cmd, **kwargs):
        kwargs['cmd'] = cmd
        self.proc.stdin.write((json.dumps(kwargs) + '\n').encode('utf-8'))
        self.proc.stdin.flush()

    def read(self):
        line = self.proc.stdout.readline()
        if not line:
            raise CivHostError('civhost closed the channel (exit %s)' % self.proc.poll())
        return json.loads(line.decode('utf-8'))

    def call(self, cmd, **kwargs):
        """Send a command; return (reply, events) where events are the {"event":...} lines before the reply."""
        self.send(cmd, **kwargs)
        events = []
        while True:
            msg = self.read()
            if 'event' in msg:
                events.append(msg)
                continue
            if not msg.get('ok'):
                raise CivHostError('%s: %s' % (msg.get('where'), msg.get('error')))
            return msg, events

    def run(self, **kwargs):
        return self.call('run', **kwargs)

    def close(self):
        if self.proc.poll() is None:
            try:
                self.call('quit')
            except (CivHostError, IOError, ValueError):
                pass
            self.proc.wait()


def _smoke(argv):
    turns = 3
    scenario = 'headless/scenarios/smoke/pangaea6.json'
    if '--turns' in argv:
        turns = int(argv[argv.index('--turns') + 1])
    if '--scenario' in argv:
        scenario = argv[argv.index('--scenario') + 1]
    runs = os.path.join(REPO, 'headless', 'out', 'runs')
    cli_dir = os.path.join(runs, 'control-smoke-cli')
    ctl_dir = os.path.join(runs, 'control-smoke-ctl')
    rc = subprocess.call([EXE, '--scenario', scenario, '--turns', str(turns), '--out', cli_dir], cwd=REPO,
                         stdout=open(os.devnull, 'w'))
    if rc != 0:
        print('CLI run failed: exit %d' % rc)
        return 1
    with CivHost(ctl_dir) as h:
        hello, _ = h.call('hello')
        game, _ = h.call('new_game', scenario=scenario)
        reply, events = h.run(turns=turns)
        turn_events = [e for e in events if e['event'] == 'turn']
        print('hello: %s, dll %s; new_game: %d plots, %d players, start %s' % (
            hello['host'], hello['dll_xxh64'], game['plots'], game['players_alive'], game['start_digest']))
        print('run: turn %d, %d turn events, reason %s' % (reply['turn'], len(turn_events), reply['reason']))
        save, _ = h.call('save')
        print('save: %s (%d bytes)' % (save['path'], save['bytes']))
    a = open(os.path.join(cli_dir, 'digest.bin'), 'rb').read()
    b = open(os.path.join(ctl_dir, 'digest.bin'), 'rb').read()
    same = a == b and len(turn_events) == turns
    print('digest.bin CLI == control: %s' % same)
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(_smoke(sys.argv[1:]))
