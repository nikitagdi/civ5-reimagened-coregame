"""Generate headless/generated-docs/ENGINE-API.md from headless/host/api/engine-api.csv.

usage: cd headless/tools; python -m civsim.enginehost.api_md [--calls <run>/api-calls.tsv] [--out <md>]
With --calls, a column shows how often the DLL called each method in that run (civhost writes api-calls.tsv).
"""
import csv
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
CSV = os.path.join(REPO, 'headless', 'host', 'api', 'engine-api.csv')
OUT = os.path.join(REPO, 'headless', 'generated-docs', 'ENGINE-API.md')

CLASSES = [
    ('sim', 'affects the simulation; hand-written in civhost (HstEngineUtility, net/HstLoopback.cpp, HstScriptSystem). '
            'A sim method without an override aborts the run (exit 4).'),
    ('query', 'returns a fixed value (the "value" column), as the exe would in a headless, all-AI game.'),
    ('sink', 'visual, sound, Steam, achievements: counted, no effect.'),
    ('gen', 'ICvUnknown plumbing (QueryInterface, Destroy).'),
]


def md_cell(text):
    return (text or '').replace('|', '\\|').replace('\n', ' ')


def main(argv):
    calls_path = argv[argv.index('--calls') + 1] if '--calls' in argv else None
    out_path = argv[argv.index('--out') + 1] if '--out' in argv else OUT
    with open(CSV, newline='', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    calls = {}
    if calls_path:
        calls_file = calls_path if os.path.exists(calls_path) else os.path.join(REPO, calls_path)
        with open(calls_file, encoding='utf-8') as f:
            for line in f:
                parts = line.rstrip('\n').split('\t')
                if len(parts) >= 3:
                    calls[parts[0]] = parts[2]
    counts = {}
    for r in rows:
        counts[(r['family'], r['class'])] = counts.get((r['family'], r['class']), 0) + 1
    families = []
    for r in rows:
        if r['family'] not in families:
            families.append(r['family'])
    lines = []
    lines.append('# Engine API (civhost)')
    lines.append('')
    lines.append('Generated from `headless/host/api/engine-api.csv` by `python -m civsim.enginehost.api_md`; edit the CSV, '
                 'then regenerate this file and the stubs (`pwsh headless/tools/build-host.ps1 -Regen`).')
    lines.append('Every virtual method of `ICvEngineUtility1-4`, `ICvUserInterface1-2` and `ICvEngineScriptSystem1` that the '
                 'gamecore DLL can call has one row.')
    if calls_path:
        lines.append('The "calls" column is the number of calls in the reference run `%s`.' % calls_path.replace('\\', '/'))
    lines.append('')
    lines.append('| Class | Meaning |')
    lines.append('|---|---|')
    for name, text in CLASSES:
        lines.append('| %s | %s |' % (name, text))
    lines.append('')
    lines.append('| Interface | sim | query | sink | gen | total |')
    lines.append('|---|---|---|---|---|---|')
    for fam in families:
        n = [counts.get((fam, c), 0) for c in ('sim', 'query', 'sink', 'gen')]
        lines.append('| %s | %d | %d | %d | %d | %d |' % (fam, n[0], n[1], n[2], n[3], sum(n)))
    lines.append('')
    lines.append('Exe RVAs are filled in where reverse engineering pinned the exe implementation (image base 0x400000); most '
                 'rows are still open.')
    for fam in families:
        lines.append('')
        lines.append('## %s' % fam)
        lines.append('')
        header = '| Method | Class | Value | Exe RVA | Note |'
        sep = '|---|---|---|---|---|'
        if calls_path:
            header = '| Method | Class | Value | Exe RVA | Calls | Note |'
            sep = '|---|---|---|---|---|---|'
        lines.append(header)
        lines.append(sep)
        for r in rows:
            if r['family'] != fam:
                continue
            cells = ['`%s`' % r['method'], r['class'], md_cell(r['value']), md_cell(r['exe_rva'])]
            if calls_path:
                cells.append(calls.get('%s::%s' % (r['family'], r['method']), ''))
            cells.append(md_cell(r['note']))
            lines.append('| ' + ' | '.join(cells) + ' |')
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d methods' % (out_path, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
