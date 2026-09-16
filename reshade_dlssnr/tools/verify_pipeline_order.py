"""Compare the refactored step tables against the pre-refactor operation order.

The oracle is the ordered list of GPU-visible / state-mutating operations in
the original PassSession::OnPresent and OnPostDlssEvaluate, read straight out
of git history. Each entry names the call as it appeared. The new step tables
are read from pipeline_graph.cpp and mapped back to the same names.

Usage (from reshade_dlssnr/):

    python tools/verify_pipeline_order.py [rev]

`rev` defaults to HEAD. Pass the commit that still had the monolithic
PassSession::OnPresent, e.g. if the refactor has since been committed:

    python tools/verify_pipeline_order.py HEAD~1

Read the two lists side by side. They will not be textually identical: a step
that absorbed several operations shows once, and the EnsureHdr / HdrBeginFrame
nodes are new (they were controller-side calls before). What must hold is that
no operation changed its ORDER relative to the others.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

GRAPH = os.path.join(ROOT, 'runtime', 'pipeline_graph.cpp')
ORIG_REL = 'reshade_dlssnr/runtime/pass_session.cpp'


def read(path):
    with open(path, encoding='utf-8', errors='replace') as handle:
        return handle.read()


def read_git_source(rev):
    """The pre-refactor pass_session.cpp, straight from git."""
    try:
        out = subprocess.run(
            ['git', 'show', '%s:%s' % (rev, ORIG_REL)],
            cwd=ROOT, capture_output=True, check=True)
    except FileNotFoundError:
        print('git not found on PATH', file=sys.stderr)
        return None
    except subprocess.CalledProcessError as exc:
        err = exc.stderr.decode('utf-8', 'replace').strip()
        print('git show %s:%s failed: %s' % (rev, ORIG_REL, err),
              file=sys.stderr)
        return None
    return out.stdout.decode('utf-8', 'replace')


def main(argv):
    rev = argv[1] if len(argv) > 1 else 'HEAD'
    if not os.path.exists(GRAPH):
        print('missing %s' % GRAPH, file=sys.stderr)
        return 1
    orig = read_git_source(rev)
    if orig is None:
        return 1
    graph = read(GRAPH)

    print('oracle: git %s:%s' % (rev, ORIG_REL))
    print()

    present_new = table(graph, 'kPresentSteps')
    post_new = table(graph, 'kPostDlssSteps')

    print('=== NEW Present step table ===')
    for i, x in enumerate(present_new, 1): print(' %2d %s' % (i, x))
    print()
    print('=== NEW PostDLSS step table ===')
    for i, x in enumerate(post_new, 1): print(' %2d %s' % (i, x))
    print()
    print('=== ORIGINAL Present operation order ===')
    for i, x in enumerate(seq(orig, 'void PassSession::OnPresent(',
                              'void PassSession::OnPostDlssEvaluate('), 1):
        print(' %2d %s' % (i, x))
    print()
    print('=== ORIGINAL PostDLSS operation order ===')
    for i, x in enumerate(seq(orig, 'void PassSession::OnPostDlssEvaluate(',
                              'void PassSession::Shutdown('), 1):
        print(' %2d %s' % (i, x))
    print()
    print('=== ORIGINAL early-exit guards (not sequential steps) ===')
    for stage, start, end in (
            ('Present', 'void PassSession::OnPresent(',
             'void PassSession::OnPostDlssEvaluate('),
            ('PostDLSS', 'void PassSession::OnPostDlssEvaluate(',
             'void PassSession::Shutdown(')):
        names = guarded(orig, start, end)
        print(' %s: %s' % (stage, ', '.join(names) if names else '(none)'))
    print()
    print('NOTE: the stages legitimately differ inside these guards. Present')
    print('      submits (SubmitAndWait) before returning on ConsumeJustRebuilt;')
    print('      PostDLSS returns without submitting. Both are reproduced')
    print('      inside the owning step - see the FIDELITY note in')
    print('      runtime/pipeline_graph.cpp.')
    return 0


# Patterns to look for inside the original bodies, in priority order. Each maps
# a source line to the name of the step that now owns it.
OP_PATTERNS = [
    ('AcquireBackbuffer(', 'AcquireBackbuffer'),
    ('runtime.Initialize(', 'ResolveDevice.runtime'),
    ('command_.Ensure(', 'ResolveDevice.cmd'),
    ('working_scale = std::clamp', 'ResolveWorkingScale'),
    ('SetRetireCallback(', 'EnsureFrameResources.retire'),
    ('EnsureFrameResources(', 'EnsureFrameResources'),
    ('command_.Reset();', 'SeedResourceStates.reset'),
    ('SeedFrameState(', 'SeedResourceStates.seed'),
    ('feature_.EnsureFeature(', 'EnsureFeature'),
    ('guides_->GetState()', 'SnapshotGuides.read'),
    ('SetResourceState(guides.', 'SeedGuideStates'),
    ('codec.Init(', 'EnsureComputeEngine'),
    ('codec.IsReady()', 'EnsureComputeEngine.ready'),
    ('model_input = backbuffer', 'ResolveModelInput.backbuffer'),
    ('EnsureScratch(', 'ResolveModelInput.scratch'),
    ('CopyResource(scratch', 'ResolveModelInput.copy'),
    ('PrepareInput(', 'PrepareModelInput'),
    ('FillEvaluateParams(', 'FillEvaluateParams'),
    ('EvaluateFeature(', 'EvaluateFeature'),
    ('UavBarrier(', 'ModelOutputBarrier'),
    ('ComposeOutput(', 'ComposeModelOutput'),
    ('MaybeRenderDebug(', 'RenderDebugView'),
    ('CopyResourceToBackbuffer(', 'WriteToBackbuffer'),
    ('CleanupFrameState(', 'Cleanup'),
    ('SubmitAndWait()', 'SubmitAndWait'),
    ('hdr->Enabled()', 'BuildHdrUniforms'),
    ('SetResourceState(output_resource', 'SeedResourceStates.output'),
    ('GetDesc()', 'ResolveDevice.desc'),
    ('CopyResource(output_resource', 'WriteToGameResource'),
]


def seq(text, start_marker, end_marker):
    """Ordered operation names in one original method body.

    Consecutive occurrences of the same name are collapsed: several
    SetResourceState(guides.*) calls on adjacent lines are one unit of work
    (one SeedGuideStates node), not three.
    """
    body = text.split(start_marker, 1)[1].split(end_marker, 1)[0]
    out = []
    for line in body.split('\n'):
        stripped = line.strip()
        if stripped.startswith('//'):
            continue
        for pattern, name in OP_PATTERNS:
            if pattern in stripped:
                if not out or out[-1] != name:
                    out.append(name)
                break
    return out


def guarded(text, start_marker, end_marker):
    """Operations that belong to a frame-abandoning guard, with their guard.

    A guard is a block whose body ends in `return;` instead of falling through
    to the rest of the method. Its operations are NOT sequential pipeline
    steps - the runner reproduces them inside the owning step - so they are
    reported separately rather than compared position by position.

    Detection walks braces one line at a time. Each open block gets a bucket;
    operations land in the innermost bucket. When a `return;` is seen, the
    innermost *enclosing* bucket is the guard body, so its contents are lifted
    out as guarded and the bucket is cleared. Operations accumulated at
    method level (depth 0) are never lifted, which is what keeps ordinary
    straight-line steps out of the result.
    """
    body = text.split(start_marker, 1)[1].split(end_marker, 1)[0]

    # buckets[i] holds operations seen inside the block that opened at depth i.
    buckets = [[]]
    adopted = []            # ops already attributed to some guard

    for raw in body.split('\n'):
        line = raw
        if line.strip().startswith('//'):
            continue
        # Drop trailing comments so braces inside them do not skew the walk.
        code = line.split('//', 1)[0]
        stripped = code.strip()

        # `return;` closes a guard: lift the innermost non-method bucket.
        if re.fullmatch(r'\}?\s*return\s*;', stripped):
            if len(buckets) > 1:
                for name in buckets[-1]:
                    adopted.append(name)
                buckets[-1] = []

        for pattern, name in OP_PATTERNS:
            if pattern in stripped:
                if not buckets[-1] or buckets[-1][-1] != name:
                    buckets[-1].append(name)
                break

        # Apply this line's brace deltas. Count on the comment-stripped text.
        depth_change = code.count('{') - code.count('}')
        if depth_change > 0:
            for _ in range(depth_change):
                buckets.append([])
        elif depth_change < 0:
            for _ in range(-depth_change):
                if len(buckets) > 1:
                    buckets.pop()

    out = []
    for name in adopted:
        if name not in out:
            out.append(name)
    return out


def table(text, name):
    """Step ids of one constexpr table in pipeline_graph.cpp."""
    body = text.split('constexpr PipelineNodeId %s[] = {' % name, 1)[1]
    body = body.split('};', 1)[0]
    return re.findall(r'PipelineNodeId::(\w+)', body)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
