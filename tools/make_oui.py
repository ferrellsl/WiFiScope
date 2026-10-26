"""Builds res/oui.txt, WiFiScope's list of hardware manufacturers, from the IEEE
registry's CSV files (https://standards-oui.ieee.org/: oui/oui.csv, oui28/mam.csv,
oui36/oui36.csv - the large, medium and small address blocks).

    python tools/make_oui.py oui.csv mam.csv oui36.csv

Each line of the output: the block's first hex digits (6, 7 or 9 of them), a
tab, and the organization's name, shortened ("TP-LINK TECHNOLOGIES CO.,LTD."
-> "TP-LINK Technologies").
"""
import csv
import os
import re
import sys

SUFFIX = re.compile(
    r'[\s,.]+(co\.?\s*,?\s*ltd|co\.?,?\s*limited|company\s+limited|corporation|incorporated|limited|'
    r'co|ltd|inc|corp|llc|l\.l\.c|gmbh|ag|s\.?a|s\.?p\.?a|b\.?v|oy|ab|pte|pty|plc|s\.?r\.?l|kg|a/s|sas|'
    r'co\.?,?\s*kg|gmbh\s*&\s*co\.?\s*kg|technology\s+co|tech\s+co)\.?$', re.I)


def tidy(name):
    # (full-width punctuation in some Chinese names)
    name = name.replace('，', ',').replace('（', '(').replace('）', ')').replace('、', ',')
    name = ' '.join(name.replace(' ', ' ').split()).strip(' ,.')
    for _ in range(4):
        shorter = SUFFIX.sub('', name).strip(' ,.')
        if shorter == name or not shorter:
            break
        name = shorter
    # SHOUTING words of more than four letters: capitalized (acronyms stay as they are)
    def word(m):
        w = m.group(0)
        return w.capitalize() if len(w) > 4 and w.isupper() else w
    return re.sub(r'[A-Za-z]+', word, name)


def main(paths):
    blocks = {}
    for path in paths:
        with open(path, encoding='utf-8', newline='') as f:
            for row in csv.DictReader(f):
                prefix = row['Assignment'].strip().upper()
                name = tidy(row['Organization Name'])
                if re.fullmatch(r'[0-9A-F]{6}|[0-9A-F]{7}|[0-9A-F]{9}', prefix) and name:
                    blocks[prefix] = name
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res', 'oui.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# IEEE registry (MA-L, MA-M, MA-S), from tools/make_oui.py\n')
        for prefix in sorted(blocks):
            f.write(prefix + '\t' + blocks[prefix] + '\n')
    print(len(blocks), 'blocks ->', os.path.normpath(out), os.path.getsize(out), 'bytes')


if __name__ == '__main__':
    main(sys.argv[1:])
