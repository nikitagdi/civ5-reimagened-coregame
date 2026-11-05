"""civsim.simrunner — scenarios, corpora, cached/parallel civhost runs (V1 cut).

CLI: python -m civsim.simrunner validate|expand|run|batch|report|show|cache|selftest   (PYTHONPATH=headless\\tools)
API: Ctx, run_scenario, run_batch, load_run, prepare (runner); resolve_targets, expand_corpus, validate (scenario).
"""
from .runner import Ctx, ValidationError, batch_exit_code, execute, load_run, prepare, run_batch, run_scenario  # noqa: F401
from .scenario import Item, expand_corpus, resolve_targets, validate  # noqa: F401
