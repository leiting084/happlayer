@echo off
rem Generate 3 HAP test clips (opaque bottom + 2 alpha layers). Requires deps\ffmpeg.
set FF=%~dp0deps\ffmpeg\bin\ffmpeg.exe
if not exist "%~dp0clips" mkdir "%~dp0clips"

rem bottom: opaque test pattern (Hap / DXT1)
"%FF%" -y -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=12" -c:v hap -format hap "%~dp0clips\layer1_bottom.mov"

rem middle: two moving translucent boxes (Hap Alpha / DXT5)
"%FF%" -y -f lavfi -i "color=black@0.0:size=960x540:rate=30:duration=12,format=rgba,drawbox=x='mod(t*120\,1120)-160':y=100:w=160:h=160:color=red@0.75:t=fill,drawbox=x='960-mod(t*80\,1080)':y=300:w=120:h=120:color=cyan@0.6:t=fill" -c:v hap -format hap_alpha "%~dp0clips\layer2_mid.mov"

rem top: swinging translucent green box, 6s no-loop (timed entrance + disappear)
"%FF%" -y -f lavfi -i "color=black@0.0:size=640x360:rate=30:duration=6,format=rgba,drawbox=x='w/2-100+80*sin(t*2)':y='h/2-75':w=200:h=150:color=lime@0.7:t=fill" -c:v hap -format hap_alpha "%~dp0clips\layer3_top.mov"

echo Done. Clips are in clips\
