#!/usr/bin/env python3
from pathlib import Path
import sys
root=Path(sys.argv[1] if len(sys.argv)>1 else '.').resolve()
cm=root/'CMakeLists.txt'; cc=root/'src/HttpServer.cpp'
if not cm.exists() or not cc.exists(): raise SystemExit('run from Tvstreamer_sat root')
s=cm.read_text(encoding='utf-8')
if 'src/OscamMiniManager.cpp' not in s:
    m='    src/HttpServer.cpp\n)'
    if m not in s: raise SystemExit('CMake marker not found')
    s=s.replace(m,'    src/HttpServer.cpp\n    src/OscamMiniManager.cpp\n)',1)
    cm.write_text(s,encoding='utf-8')
s=cc.read_text(encoding='utf-8')
if '#include "OscamMiniManager.h"' not in s:
    m='#include "CardManager.h"\n'
    if m not in s: raise SystemExit('include marker not found')
    s=s.replace(m,m+'#include "OscamMiniManager.h"\n',1)
if 'target == "/oscam-mini"' not in s:
    m='            } else if (target == "/api/interfaces") {'
    ins='''            } else if (target == "/oscam-mini") {
                res.set(http::field::content_type, "text/html; charset=UTF-8");
                res.body() = OscamMiniManager::instance().renderPage();
            } else if (target == "/api/oscam-mini/status") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().statusJson();
            } else if (target == "/api/oscam-mini/settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().settingsJson();
            } else if (target == "/api/interfaces") {'''
    if m not in s: raise SystemExit('GET marker not found')
    s=s.replace(m,ins,1)
if 'target == "/api/oscam-mini/save"' not in s:
    m='            if (target == "/api/save-config") {'
    ins='''            if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().saveSettingsJson(req.body());
            } else if (target == "/api/oscam-mini/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().serviceActionJson(req.body());
            } else if (target == "/api/save-config") {'''
    if m not in s: raise SystemExit('POST marker not found')
    s=s.replace(m,ins,1)
cc.write_text(s,encoding='utf-8')
print('OSCam-mini module integrated')
