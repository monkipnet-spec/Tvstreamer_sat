Separate OSCam-mini module.
Backend: src/OscamMiniManager.h/.cpp
Own config only: /opt/TVStreammerSAT5/oscam-mini/config
Web UI: /oscam-mini
API: /api/oscam-mini/status, /settings, /save, /action
Run once after copying: python3 scripts/integrate_oscam_mini_module.py .
