#!/usr/bin/env python3
"""The release notes of one version, from CHANGELOG.md.

usage: release_notes.py <version> [CHANGELOG.md]

Prints the section "## <version> ..." without its heading, with the hard
line breaks inside paragraphs and list items joined (GitHub keeps them in
release notes). Exit code 1 if the changelog has no such section.
"""
import sys


def notes(version, text):
    lines = text.split('\n')
    start = None
    for i, line in enumerate(lines):
        if line.startswith('## '):
            if start is not None:
                lines = lines[:i]
                break
            head = line[3:].split()
            if head and head[0] == version:
                start = i + 1
    if start is None:
        return None
    out = []
    for line in lines[start:]:
        joins = (out and line.strip() and out[-1].strip()
                 and not line.lstrip().startswith(('- ', '#'))
                 and not out[-1].startswith('#'))
        if joins:
            out[-1] = out[-1].rstrip() + ' ' + line.strip()
        else:
            out.append(line)
    return '\n'.join(out).strip() + '\n'


if __name__ == '__main__':
    path = sys.argv[2] if len(sys.argv) > 2 else 'CHANGELOG.md'
    result = notes(sys.argv[1], open(path, encoding='utf-8').read())
    if result is None:
        sys.exit('no section for %s in %s' % (sys.argv[1], path))
    sys.stdout.write(result)
