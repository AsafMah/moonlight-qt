"""Merge a conflicted .ts file structurally during a git merge.

Starts from HEAD's whole file and adds every message of MERGE_HEAD's file
that HEAD lacks, into the same <context>. Then checks that every
(context, source, comment) of both sides is present.

Usage (from the repository root): python -I merge_ts.py <file.ts>...
"""
import html
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8')

HELPERS = r'''
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')


def esc(text):
    return (text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
            .replace("'", '&apos;').replace('"', '&quot;'))


def message(source, translation, comment=None):
    out = '    <message>\n        <source>%s</source>\n' % esc(source)
    if comment:
        out += '        <comment>%s</comment>\n' % esc(comment)
    out += '        <translation>%s</translation>\n    </message>\n' % esc(translation)
    return out


def context_span(s, name):
    start = s.find('<name>%s</name>' % name)
    if start < 0:
        return None
    end = s.index('</context>', start)
    return start, end


def has_message(s, name, source, comment=None):
    span = context_span(s, name)
    if span is None:
        return False
    body = s[span[0]:span[1]]
    pattern = r'<source>%s</source>' % re.escape(esc(source))
    if comment:
        pattern += r'\s*<comment>%s</comment>' % re.escape(esc(comment))
    else:
        pattern += r'(?!\s*<comment>)'
    return re.search(pattern, body) is not None


def add(s, name, source, translation, comment=None):
    if has_message(s, name, source, comment):
        return s
    span = context_span(s, name)
    if span is None:
        new = '<context>\n    <name>%s</name>\n%s</context>\n' % (name, message(source, translation, comment))
        i = s.rindex('</TS>')
        return s[:i] + new + s[i:]
    return s[:span[1]] + message(source, translation, comment) + s[span[1]:]


def replace(s, name, old, new, translation):
    span = context_span(s, name)
    body = s[span[0]:span[1]]
    pattern = re.compile(r'<source>%s</source>(\s*)<translation[^>]*>.*?</translation>' % re.escape(esc(old)), re.S)
    assert pattern.search(body), (name, old)
    body = pattern.sub(lambda m: '<source>%s</source>%s<translation>%s</translation>' % (esc(new), m.group(1), esc(translation)), body, count=1)
    return s[:span[0]] + body + s[span[1]:]


'''
root = os.getcwd()
paths = sys.argv[1:]

ns = {}
exec(HELPERS, ns)

context_re = re.compile(r'<context>\s*<name>(.*?)</name>(.*?)</context>', re.S)
message_re = re.compile(r'<message[^>]*>(.*?)</message>', re.S)


def messages(text):
    out = []
    for c in context_re.finditer(text):
        for m in message_re.finditer(c.group(2)):
            body = m.group(1)
            source = re.search(r'<source>(.*?)</source>', body, re.S)
            if not source:
                continue
            comment = re.search(r'<comment>(.*?)</comment>', body, re.S)
            translation = re.search(r'<translation[^>]*>(.*?)</translation>', body, re.S)
            out.append((c.group(1), html.unescape(source.group(1)),
                        html.unescape(comment.group(1)) if comment else None,
                        html.unescape(translation.group(1)) if translation else ''))
    return out


def show(rev, path):
    return subprocess.run(['git', 'show', f'{rev}:{path}'], capture_output=True, text=True,
                          encoding='utf-8', check=True).stdout


for path in paths:
    ours = show('HEAD', path)
    theirs = show('MERGE_HEAD', path)
    result = ours
    added = 0
    present = {(c, s, k) for c, s, k, _ in messages(result)}
    for context, source, comment, translation in messages(theirs):
        if (context, source, comment) not in present:
            result = ns['add'](result, context, source, translation, comment)
            present.add((context, source, comment))
            added += 1
    final = {(c, s, k) for c, s, k, _ in messages(result)}
    for side, text in (('HEAD', ours), ('MERGE_HEAD', theirs)):
        missing = {(c, s, k) for c, s, k, _ in messages(text)} - final
        assert not missing, (path, side, list(missing)[:5])
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(result)
    print(path, 'added', added, 'messages from MERGE_HEAD')
