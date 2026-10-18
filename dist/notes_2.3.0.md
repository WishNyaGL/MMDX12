<img src="https://raw.githubusercontent.com/digital8150/MMDX12/master/docs/media/bloom-2.3.0.jpg" alt="MMDX12 2.3.0 - cinema convolution bloom" width="100%">

# MMDX12 2.3.0 — DLSS Ray Reconstruction & Frame Generation, Cinematic Bloom

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **DLSS 프레임 생성으로 플레이 화면을 더 부드럽게 볼 수 있어요**: 2x(RTX 40 이상), 3x·4x(RTX 50)를 고를 수 있어요. 플레이 화면에서만 동작하고, 로비·스튜디오·영상 렌더에서는 꺼져요.
- **DLSS 레이 재구성으로 레이 트레이싱·패스 트레이싱 화면이 더 깨끗해져요**: DLSS를 켠 RT·PT 모드에서 노이즈 제거와 반사·AO 정리를 DLSS가 맡아, 움직일 때 번짐과 얼룩이 줄어요.
- **DLSS 모델을 직접 고를 수 있어요**: 자동, CNN, 트랜스포머, 트랜스포머 2세대 중에서 선택해요(DLSS 310.9.16).
- **블룸 모양을 원하는 대로 조절할 수 있어요**: 반경, 섞는 방식(더하기 / 에너지 보존: 화면이 뿌옇게 밝아지지 않음), 깊이 마스크를 플레이 패널, 로비 세부 설정, 스튜디오 효과 탭에서 바꿀 수 있어요.
- **컨볼루션 블룸에 시네마·별빛·글로우·아나모픽 프리셋이 생겼어요**: 빛줄기 수·길이·세기, 글로우 반경·세기, 색 분산을 슬라이더로 다듬을 수 있어요. 기본 모양은 은은한 빛번짐 위에 짧은 별빛이 얹히는 "시네마"로 바뀌었고, 예전 모양은 "별빛"이에요.

### 🐞 수정 및 개선
- **영상 렌더가 더 빨라졌어요**: 프레임 읽기와 인코딩이 렌더링과 겹쳐서 진행돼요. 미리보기 창에는 실제로 저장되는 프레임이 그대로 보여요.
- **고품질(GI) 영상 렌더에서 아이 어댑테이션이 깜빡이던 문제를 고쳤어요**: 시간에 따라 변하는 화면 효과의 상태가 매 프레임 초기화되어 노출이 프레임마다 튀었어요. 이제 부드럽게 적응해요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- 라이트 버전에서 DLSS를 이미 설치했다면, 레이 재구성과 프레임 생성에 필요한 파일 두 개를 위해 DLSS 구성요소를 한 번 더 받아 주세요.

## English
### ✨ New
- **DLSS Frame Generation makes play scenes smoother**: choose 2x (RTX 40 or newer) or 3x / 4x (RTX 50). It runs in play scenes only; the lobby, the studio and video renders keep it off.
- **DLSS Ray Reconstruction cleans up the ray-traced and path-traced views**: with DLSS on in RT / PT mode, DLSS takes over denoising and the reflection / AO filtering, so motion leaves fewer smears and blotches.
- **You can now pick the DLSS model**: auto, CNN, transformer or transformer 2nd gen (DLSS 310.9.16).
- **The bloom look is now adjustable**: radius, blend (additive / energy conserving, which keeps the screen from washing out) and a depth mask, in the play panel, the lobby's detail settings and the studio's effects tab.
- **Convolution bloom gains Cinema, Starburst, Glow and Anamorphic presets**: fine-tune the ray count, length and intensity, the glow radius and intensity, and dispersion with sliders. The default is now "Cinema" (short star glints over a soft glow); the old look is "Starburst".

### 🐞 Fixes & Improvements
- **Video renders are faster**: reading frames back and encoding now overlap with rendering, and the preview shows exactly the frame that is written.
- **Fixed eye adaptation flickering in high-quality (GI) video renders**: the state of time-based screen effects was reset on every frame, so the exposure jumped from frame to frame. It now adapts smoothly.

### 📦 Notes
- Available through the automatic update (only the changed files are downloaded).
- Lite version users who already installed DLSS: fetch the DLSS component once more to get the two files Ray Reconstruction and Frame Generation need.

## 日本語
### ✨ 新機能
- **DLSS フレーム生成でプレイ画面がより滑らかになります**: 2x (RTX 40 以降)、3x・4x (RTX 50) を選べます。プレイ画面でのみ動作し、ロビー・スタジオ・動画レンダーではオフです。
- **DLSS レイ再構成でレイトレーシング・パストレーシングの画面がきれいになります**: DLSS をオンにした RT・PT モードでは、ノイズ除去と反射・AO の処理を DLSS が担当し、動いたときのにじみやムラが減ります。
- **DLSS モデルを選べるようになりました**: 自動、CNN、トランスフォーマー、トランスフォーマー第 2 世代から選択できます (DLSS 310.9.16)。
- **ブルームの見た目を調整できるようになりました**: 半径、合成方法 (加算 / エネルギー保存: 画面が白っぽくならない)、深度マスクを、プレイパネル、ロビーの詳細設定、スタジオのエフェクトタブで変更できます。
- **コンボリューションブルームにシネマ・スターバースト・グロー・アナモルフィックのプリセットが加わりました**: 光条の数・長さ・強さ、グローの半径・強さ、色分散をスライダーで調整できます。既定の見た目は柔らかなグローに短い光条が乗る「シネマ」に変わり、以前の見た目は「スターバースト」です。

### 🐞 修正と改善
- **動画レンダーが速くなりました**: フレームの読み出しとエンコードがレンダリングと並行して進みます。プレビューには実際に書き出されるフレームがそのまま表示されます。
- **高品質 (GI) 動画レンダーで明暗順応 (アイアダプテーション) がちらつく問題を修正しました**: 時間で変化する画面効果の状態が毎フレーム初期化され、露出がフレームごとに跳ねていました。今は滑らかに順応します。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみダウンロードします)。
- ライト版で DLSS をインストール済みの場合は、レイ再構成とフレーム生成に必要な 2 つのファイルのため、DLSS コンポーネントをもう一度取得してください。

## 简体中文
### ✨ 新功能
- **DLSS 帧生成让播放画面更流畅**: 可选择 2x (RTX 40 及以上) 或 3x / 4x (RTX 50)。仅在播放画面中生效,大厅、工作室和视频渲染中保持关闭。
- **DLSS 光线重建让光线追踪和路径追踪画面更干净**: 在开启 DLSS 的 RT / PT 模式下,由 DLSS 负责降噪以及反射 / AO 的处理,运动时的拖影和斑块更少。
- **现在可以选择 DLSS 模型**: 自动、CNN、Transformer 或第二代 Transformer (DLSS 310.9.16)。
- **泛光效果现在可以调节**: 半径、混合方式 (叠加 / 能量守恒: 画面不会发白)、深度遮罩,可在播放面板、大厅的详细设置和工作室的效果标签中更改。
- **卷积泛光新增电影、星芒、辉光、变形宽银幕预设**: 可用滑块微调光芒数量、长度和强度,辉光半径和强度,以及色散。默认效果改为柔和辉光上带有短星芒的"电影",旧效果为"星芒"。

### 🐞 修复与改进
- **视频渲染更快了**: 帧读取和编码与渲染同时进行,预览窗口显示的正是实际写入的帧。
- **修复了高质量 (GI) 视频渲染中人眼适应效果闪烁的问题**: 随时间变化的画面效果的状态每帧都被重置,导致曝光逐帧跳动。现在会平滑适应。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件)。
- 轻量版用户如已安装 DLSS: 请再获取一次 DLSS 组件,以取得光线重建和帧生成所需的两个文件。
