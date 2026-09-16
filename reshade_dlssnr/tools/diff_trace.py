"""Diff two DLSSNR_TRACE logs to verify a pipeline change is behavior-preserving.

Why this exists: compile, link and a table-order comparison are all static.
They cannot see whether the addon, at runtime, records the same commands in
the same order. A trace diff can. This is the only check that closes that gap,
and it is the acceptance gate for the data-driven pipeline refactor.

Capture:
    1. Build the revision you want as a baseline.
    2. Run the game with DLSSNR_TRACE set, e.g.
         set DLSSNR_TRACE=%TEMP%\\trace_before.log
    3. Build the revision under test, run again with a second path.
    4. python tools/diff_trace.py trace_before.log trace_after.log

Resource pointers are printed as stable first-seen indices, not raw addresses,
because COM heaps recycle addresses between runs. That is what makes two runs
comparable at all.

Bound the capture by keeping the run short and deterministic. There is no
frame limit in the tracer itself (only DLSSNR_TRACE is read), and traces grow
fast - every transition, barrier and dispatch is a line - so an unbounded
capture from a menu screen makes a diff unreadable. Use a fixed scene: same
save, same camera, same duration for both runs.

Exit status: 0 if the traces match, 1 if they differ, 2 on usage/IO error or
on an empty trace.
"""
import argparse
import os
import re
import sys

# Fields that legitimately vary between runs and carry no ordering meaning.
# Everything else is compared verbatim, including the per-line sequence
# number - a gap there is a real difference worth surfacing.
NOISY = [
    (re.compile(r'0x[0-9a-fA-F]{6,}'), '<addr>'),
    (re.compile(r'\b\d+\.\d{3,}ms\b'), '<time>'),
]


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace') as handle:
        return [normalise(l.rstrip('\n')) for l in handle if l.strip()]


def normalise(line):
    for pattern, replacement in NOISY:
        line = pattern.sub(replacement, line)
    return line


def split_line(line):
    """(sequence, category, rest) for a trace line, or None if unparseable.

    Format is written by pipeline_trace.cpp:Line():
        %06llu %-3s %-28s obj=%d <formatted>
    """
    parts = line.split(None, 3)
    if len(parts) < 4:
        return None
    seq, category, function, rest = parts
    if not seq.isdigit():
        return None
    return (int(seq), category, function, rest)


def first_divergence(before, after):
    """Index of the first differing line, or None when the traces match.

    Compares line by line rather than by sequence number: the question is
    whether the two revisions emitted the same stream, and position is what
    defines a stream.
    """
    for i in range(min(len(before), len(after))):
        if before[i] != after[i]:
            return i
    if len(before) != len(after):
        return min(len(before), len(after))
    return None


def context(lines, index, span):
    lo = max(0, index - span)
    hi = min(len(lines), index + span + 1)
    return [(i, lines[i]) for i in range(lo, hi)]


def print_context(label, lines, index, span):
    print('  %s:' % label)
    for i, line in context(lines, index, span):
        marker = '>>' if i == index else '  '
        print('   %s [%4d] %s' % (marker, i + 1, line))


def main(argv):
    parser = argparse.ArgumentParser(
        description='Diff two DLSSNR_TRACE logs.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('before', help='baseline trace (pre-change)')
    parser.add_argument('after', help='trace from the revision under test')
    parser.add_argument('-c', '--context', type=int, default=3,
                        help='lines of context around the divergence '
                             '(default 3)')
    parser.add_argument('--summary', action='store_true',
                        help='count operations by type in each trace, and '
                             'exit 0. Useful before diffing, to check the two '
                             'captures are even comparable.')
    args = parser.parse_args(argv[1:])

    for path in (args.before, args.after):
        if not os.path.exists(path):
            print('missing trace: %s' % path, file=sys.stderr)
            return 2

    before = read_lines(args.before)
    after = read_lines(args.after)

    if not before or not after:
        print('one or both traces are empty - was DLSSNR_TRACE set for both '
              'runs?', file=sys.stderr)
        print('an empty trace is not evidence of a match.', file=sys.stderr)
        return 2

    print('before: %-48s %6d lines' % (args.before, len(before)))
    print('after : %-48s %6d lines' % (args.after, len(after)))
    print()

    if args.summary:
        print_operation_summary(before, after)
        return 0

    index = first_divergence(before, after)

    if index is None:
        print('MATCH: %d lines identical, in order.' % len(before))
        print()
        print('The two revisions record the same commands in the same order.')
        return 0

    print('DIVERGENCE at line %d of %d/%d.'
          % (index + 1, len(before), len(after)))
    print()
    print_context('before', before, index, args.context)
    print()
    print_context('after', after, index, args.context)
    print()

    # A missing line on one side is the interesting case; report it plainly
    # rather than making the reader work it out from the context.
    b = before[index] if index < len(before) else None
    a = after[index] if index < len(after) else None
    if b is None:
        print('The before trace ENDS here; the after trace continues.')
        print('The refactor added operations, or the capture was longer.')
    elif a is None:
        print('The after trace ENDS here; the before trace continues.')
        print('The refactor dropped operations, or the capture was shorter.')
    else:
        sb, sa = split_line(b), split_line(a)
        if sb and sa:
            if sb[3] != sa[3]:
                print('Same position, different command:')
                print('  before: %s %s' % (sb[2], sb[3]))
                print('  after : %s %s' % (sa[2], sa[3]))
            elif sb[2] != sa[2]:
                print('Same position, same payload, different function:')
                print('  before: %s' % sb[2])
                print('  after : %s' % sa[2])
                print('This is expected when a step was renamed or a call was')
                print('moved between steps; it is only harmless if the payload')
                print('is genuinely identical.')
    print()
    print('Note: everything after this point is desynchronised, so ignore the')
    print('rest of the file until this difference is explained. One early')
    print('reorder inflates the total count enormously and means nothing.')
    return 1


def print_operation_summary(before, after):
    """Count lines per category+function, to sanity-check comparability."""
    def tally(lines):
        counts = {}
        for line in lines:
            parsed = split_line(line)
            if parsed is None:
                counts['(unparseable)'] = counts.get('(unparseable)', 0) + 1
                continue
            _, category, function, _ = parsed
            key = '%s %s' % (category, function)
            counts[key] = counts.get(key, 0) + 1
        return counts

    cb, ca = tally(before), tally(after)
    keys = sorted(set(cb) | set(ca))

    width = max((len(k) for k in keys), default=10)
    print('%-*s %10s %10s' % (width, 'operation', 'before', 'after'))
    print('-' * (width + 22))
    for key in keys:
        n_before, n_after = cb.get(key, 0), ca.get(key, 0)
        flag = '' if n_before == n_after else '   <-- differs'
        print('%-*s %10d %10d%s' % (width, key, n_before, n_after, flag))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
