@echo off
chcp 65001 >nul

echo.
echo 正在推送到 GitHub...
echo.

git add .
git commit -m "Update: 大麥蟲智能養殖監控系統"
git push

echo.
echo ✅ 推送完成！
echo.
pause

