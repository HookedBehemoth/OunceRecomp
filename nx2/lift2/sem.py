"""The semantic templates of every table, by name: sem_int.py's and those of every module sem_*.py of this package
(each has TEMPLATES: name -> function(builder, fields, pc, params)). A template registered as None is decoded but not
lifted yet: build_ir.scope() passes such functions over and names the template. CLASS gives each template's class
(the module it comes from), so that a build can take some classes only (build_ir.Program's classes).
"""
import importlib
import os
import pkgutil

from . import sem_int

TEMPLATES = dict(sem_int.TEMPLATES)
CLASS = {name: "int" for name in TEMPLATES}   # template -> its class: the module's name after sem_ (int, fp, neon, ...)
for _m in sorted(pkgutil.iter_modules([os.path.dirname(__file__)])):
    if _m.name.startswith("sem_") and _m.name != "sem_int":
        _t = importlib.import_module("." + _m.name, __package__).TEMPLATES
        TEMPLATES.update(_t)
        CLASS.update({name: _m.name[4:] for name in _t})
