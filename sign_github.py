from playwright.sync_api import sync_playwright
import json

# 本地Cookie持久化文件路径
COOKIE_FILE = "github_login_cookies.json"

def cookie_auto_login():
    with sync_playwright() as p:
        # 启动有头浏览器，可直观看到登录效果
        browser = p.chromium.launch(headless=False)
        # 读取之前手动保存的登录态Cookie
        #with open(COOKIE_FILE, "r", encoding="utf-8") as f:
        #    login_cookies = json.load(f)
        login_cookies = "wYT8SNyOiTplFr_IKpuXq1WMTRI89CW-QB_IouaLENFQwvHB"
        # 新建浏览器上下文并注入所有登录Cookie
        context = browser.new_context()
        context.add_cookies([{
            "name": "user_session",
            "value": login_cookies,
            "domain": ".github.com",
            "path": "/"
}])        
        # 直接打开GitHub主页，无需任何登录操作
        page = context.new_page()
        page.goto("https://github.com")
        print("✅ Cookie注入完成，已直接进入GitHub已登录状态")
        
        # 保持页面打开5秒，方便你确认登录状态
        page.wait_for_timeout(5000)
        browser.close()

if __name__ == "__main__":
    cookie_auto_login()
