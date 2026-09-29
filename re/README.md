# re/ — 逆向分析中间产物（已清理）

本目录原本存放逆向过程中下载的**官方 MCHOSE HUB 前端包**（约 11 MB 明文 JS）。
为**避免再分发厂商代码**，这些副本已从仓库中删除，只保留可复现的获取路径。

## 原始分析对象

| 文件 | 用途 |
| :--- | :--- |
| `https://cdn.mchose.com.cn/customPage/14/index.html` | A7 系列鼠标页面入口 |
| 其引用的 `./assets/main-*.js`（约 10 MB） | **命令表、字段 parser、读写决策的全部定义** |
| `D:\mhub\MCHOSE HUB\resources\app\out\main\index.jsc` | 官方 Electron 主进程 V8 字节码（仅用于字符串提取） |
| `%APPDATA%\MCHOSEHUB\files\config\mc_main_store_key.json` | 官方软件缓存的设备状态，用于交叉验证电量/固件 |

## 复现方法

```powershell
# 1) 取页面入口，找出 assets 里的 main-*.js 名称
curl.exe -s -L "https://cdn.mchose.com.cn/customPage/14/index.html"

# 2) 下载该 bundle，然后检索协议关键字
curl.exe -s -L -o p14.js "https://cdn.mchose.com.cn/customPage/14/assets/<main-xxx>.js"

# 3) 关键检索点（在 p14.js 中）
#    rateMap$1= / lodMap$1= / dpiMap$2=        预设表
#    mouseDiffConfig$1=                        机型 → pid / 档位数 / dpiMax
#    readMap$1=                                读命令表
#    setMap$1=                                 写命令表
#    *.Parser=                                 字段布局
#    receiveFeatureReport / sendFeatureReport  HID 收发路径
```

页面包编号与产品线相关（A7 系列 = `14`）。若官方改版导致 404，
可在官方软件的日志 `%APPDATA%\MCHOSEHUB\files\log\*.txt` 中搜索
`renderer loaded from remote:` 得到当前实际使用的包路径。

## 工具

本目录已不保留任何二进制分析产物。所有需要的工具源码都在 `tools/`，
按 README 中的命令自行编译即可重新生成。
