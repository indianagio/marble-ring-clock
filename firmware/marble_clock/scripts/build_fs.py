# PlatformIO pre-script: copies data/ folder to LittleFS image
import os
Import('env')

# This just ensures the data folder exists
data_dir = os.path.join(env['PROJECT_DIR'], 'data')
if not os.path.exists(data_dir):
    os.makedirs(data_dir)
    print('[build_fs] Created data/ directory')
else:
    print('[build_fs] data/ directory OK')
