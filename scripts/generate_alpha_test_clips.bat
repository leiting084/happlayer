@echo off
chcp 65001 >nul
rem 生成「多层透明叠加」测试素材（HAP / Hap Alpha，1280x720 5秒 30fps）
rem 用项目内完整 ffmpeg（deps\ffmpeg\bin），精简交付包里的 ffmpeg 缺 avfilter 跑不了
set FF=%~dp0..\deps\ffmpeg\bin\ffmpeg.exe
set OUT=%~dp0..\clips_alpha
if not exist "%OUT%" mkdir "%OUT%"

echo [1/5] 背景层：不透明，带细节图案
"%FF%" -y -f lavfi -i "testsrc2=s=1280x720:d=5:r=30" -c:v hap -format hap "%OUT%\01_bg_opaque.mov"

echo [2/5] 硬边透明层：红色圆形，圆外全透明
"%FF%" -y -f lavfi -i "color=c=red:s=1280x720:d=5:r=30" -f lavfi -i "nullsrc=s=1280x720:d=5:r=30" -filter_complex "[0:v]format=rgb24[rgb];[1:v]format=gray,geq=lum='if(lt(hypot(X-640,Y-360),220),255,0)'[al];[rgb][al]alphamerge,format=rgba" -c:v hap -format hap_alpha "%OUT%\02_circle_hardedge.mov"

echo [3/5] 渐变透明层：绿色，左透明 右不透明
"%FF%" -y -f lavfi -i "color=c=green:s=1280x720:d=5:r=30" -f lavfi -i "nullsrc=s=1280x720:d=5:r=30" -filter_complex "[0:v]format=rgb24[rgb];[1:v]format=gray,geq=lum='255*X/W'[al];[rgb][al]alphamerge,format=rgba" -c:v hap -format hap_alpha "%OUT%\03_gradient.mov"

echo [4/5] 半透明层：蓝色，整块 50%% 透明
"%FF%" -y -f lavfi -i "color=c=blue:s=1280x720:d=5:r=30" -f lavfi -i "nullsrc=s=1280x720:d=5:r=30" -filter_complex "[0:v]format=rgb24[rgb];[1:v]format=gray,geq=lum='128'[al];[rgb][al]alphamerge,format=rgba" -c:v hap -format hap_alpha "%OUT%\04_half_transparent.mov"

echo [5/5] 棋盘透明层：细节图案 RGB + 40px 棋盘 alpha
"%FF%" -y -f lavfi -i "testsrc2=s=1280x720:d=5:r=30" -f lavfi -i "nullsrc=s=1280x720:d=5:r=30" -filter_complex "[0:v]format=rgb24[rgb];[1:v]format=gray,geq=lum='if(lt(mod(floor(X/40)+floor(Y/40),2),1),255,0)'[al];[rgb][al]alphamerge,format=rgba" -c:v hap -format hap_alpha "%OUT%\05_checker_detail.mov"

echo [参考图] 5 层按 alpha 叠加的预期效果
"%FF%" -y -i "%OUT%\01_bg_opaque.mov" -i "%OUT%\02_circle_hardedge.mov" -i "%OUT%\03_gradient.mov" -i "%OUT%\04_half_transparent.mov" -i "%OUT%\05_checker_detail.mov" -filter_complex "[0:v]format=rgba[o0];[o0][1:v]overlay[o1];[o1][2:v]overlay[o2];[o2][3:v]overlay[o3];[o3][4:v]overlay" -frames:v 1 -update 1 -pix_fmt rgba "%OUT%\reference_5layers.png"

echo 完成，素材在 %OUT%
pause
