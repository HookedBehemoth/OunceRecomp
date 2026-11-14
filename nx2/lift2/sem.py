"""The semantic templates of every table, by name: sem_int.py's and those of every module sem_*.py of this package
(each has TEMPLATES: name -> function(builder, fields, pc, params)). A template registered as None is decoded but not
lifted yet: build_ir.scope() passes such functions over and names the template.
"""
import importlib
import os
import pkgutil

from . import sem_int

TEMPLATES = dict(sem_int.TEMPLATES)
for _m in sorted(pkgutil.iter_modules([os.path.dirname(__file__)])):
    if _m.name.startswith("sem_") and _m.name != "sem_int":
        TEMPLATES.update(importlib.import_module("." + _m.name, __package__).TEMPLATES)
