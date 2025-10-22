@echo off
chcp 65001
echo ========================================
echo 大麥蟲智能養殖監控系統 - GitHub 推送工具
echo ========================================
echo.

REM 檢查是否已經初始化 git
if not exist .git (
    echo [1/5] 初始化 Git 儲存庫...
    git init
    echo.
) else (
    echo [1/5] Git 儲存庫已存在，跳過初始化
    echo.
)

echo [2/5] 將所有檔案加入暫存區...
git add .
echo.

echo [3/5] 提交變更...
git commit -m "Initial commit: 大麥蟲智能養殖監控系統"
echo.

echo [4/5] 設定主分支為 main...
git branch -M main
echo.

echo [5/5] 準備推送到 GitHub...
echo.
echo ⚠️ 請注意：
echo 1. 你需要先在 GitHub 上創建一個新的儲存庫
echo 2. 不要初始化 README（因為我們已經有了）
echo 3. 複製儲存庫的 URL（例如：https://github.com/你的帳號/儲存庫名稱.git）
echo.
set /p REPO_URL="請輸入你的 GitHub 儲存庫 URL: "

REM 檢查是否已經設定 remote
git remote | findstr "origin" >nul
if %errorlevel% equ 0 (
    echo.
    echo 已存在 origin，移除舊的設定...
    git remote remove origin
)

echo.
echo 連接到遠端儲存庫...
git remote add origin %REPO_URL%
echo.

echo 推送到 GitHub...
git push -u origin main
echo.

echo ========================================
echo ✅ 完成！
echo.
echo 接下來啟用 GitHub Pages：
echo 1. 前往你的儲存庫頁面
echo 2. 點擊 Settings ^> Pages
echo 3. Source 選擇 main 分支，資料夾選擇 / (root)
echo 4. 點擊 Save
echo 5. 等待幾分鐘後，你的網站就會上線！
echo ========================================
pause

