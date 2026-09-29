# 如何上传到 GitHub

仓库**已经在本地准备好了**：`main` 分支、1 个提交、24 个文件、工作区干净。
只差"创建远程仓库 + push"这一步——这一步需要**你的 GitHub 账号凭据**，我无法代做。

先在本地确认状态：

```cmd
cd mchose-tray
git log --oneline          :: 应看到 1 个提交 9ac0768
git status                 :: 应为 "nothing to commit, working tree clean"
```

---

## 方案 A：网页建仓 + 命令行 push（推荐，最简单）

1. 打开 <https://github.com/new>
2. **Repository name** 填 `mchose-tray`
3. **不要**勾选 "Add a README file"、不要加 .gitignore、不要选 license
   （本地已经有这些文件，勾了会导致 push 冲突）
4. 点 **Create repository**

然后执行（把 `<你的用户名>` 换成你的 GitHub 用户名）：

```cmd
cd mchose-tray
git remote add origin https://github.com/<你的用户名>/mchose-tray.git
git push -u origin main
```

第一次 push 会由 **Git Credential Manager** 弹出浏览器让你登录 GitHub 并授权，
之后凭据会被记住，不用再登。你机器上已安装 GCM，无需额外配置。

---

## 方案 B：用 GitHub CLI 一步到位

本机**没有**安装 `gh`。若想用它（能同时建仓和推送）：

```cmd
winget install --id GitHub.cli
:: 装完需要新开一个终端，然后：
gh auth login                                     :: 按提示用浏览器登录
cd mchose-tray
gh repo create mchose-tray --public --source . --remote origin --push
```

---

## 方案 C：SSH（如果你已配好 SSH key）

本机 `~/.ssh` 下**只有 known_hosts，没有密钥**，需要先生成并添加到 GitHub：

```cmd
ssh-keygen -t ed25519 -C "your@email"
type %USERPROFILE%\.ssh\id_ed25519.pub
:: 把输出粘贴到 https://github.com/settings/keys
cd mchose-tray
git remote add origin git@github.com:<你的用户名>/mchose-tray.git
git push -u origin main
```

---

## 推送前建议顺手改掉的两处

### 1. 提交者身份

本地 git 没有全局配置，我给这一个仓库设了占位身份：

```
user.name  = zxz33
user.email = zxz33@users.noreply.github.com
```

想改成你自己的（改完需要重写这个提交）：

```cmd
git config user.name  "你的名字"
git config user.email "你的邮箱"
git commit --amend --reset-author --no-edit
```

### 2. LICENSE 里的版权行

`LICENSE` 第 3 行目前是 `Copyright (c) 2026 mchose-tray contributors`，可改成你的名字或 ID。

---

## 关于二进制文件

`.gitignore` 已排除 `bin/`、`*.exe`、日志与中间产物——**仓库里只有源码、文档和工具**。
想发布可执行文件，建议走 GitHub Releases 而不是提交进仓库：

```cmd
:: 先打一个 tag
git tag -a v0.1.0 -m "mchose-tray v0.1.0"
git push origin v0.1.0
```

然后在仓库的 **Releases → Draft a new release** 里选择该 tag，把
`bin\mchose-tray.exe` 作为附件上传。

> 注意：直接上传的 exe 在别人机器上同样要放到普通目录运行。
> README 里已写明这个"Low 完整性标签会导致托盘图标注册失败"的坑。

---

## 推送后的自检清单

- [ ] 仓库首页 README 正常渲染（表格、代码块、相对链接 `docs/PROTOCOL.md` 可点开）
- [ ] `docs/PROTOCOL.md` 与 `docs/CODE-REVIEW-2025.md` 能看到
- [ ] 确认**没有**误传厂商代码：`re/` 下应只有 `README.md`
- [ ] 仓库描述建议填：
      `Lightweight Win32 tray console for MCHOSE gaming mice — independently reverse-engineered HID protocol`
- [ ] 建议加 topics：`deepseek-harness` `dsh-plugin` 之外，用
      `windows` `tray` `hid` `mchose` `reverse-engineering` `win32` `cpp17`
