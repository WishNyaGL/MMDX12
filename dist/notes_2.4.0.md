<img src="https://raw.githubusercontent.com/digital8150/MMDX12/master/docs/media/snow-2.4.0.jpg" alt="MMDX12 2.4.0 - Snow world effect" width="100%">

# MMDX12 2.4.0 — MMDX Effects: World Effects, Snow, Open Gallery

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **셰이더 팩이 "MMDX 이펙트"로 새로워졌어요**: 표면 셰이더, 화면 효과, 월드 이펙트, 룩 네 가지로 나뉘고, 로비의 탭 이름도 "이펙트"예요.
- **월드 이펙트와 기본 "Snow"가 추가됐어요**: 눈이 내리고, 댄서의 발이 닿은 자리에는 눈이 쌓여 발자국이 남으며, 발밑에서 눈이 튀어요. 재생 모드에서는 화면 효과 패널에서 켜고, 스튜디오에서는 장면에 넣어 값마다 키를 찍을 수 있어요.
- **이펙트 값을 세밀하게 다룰 수 있어요**: 이펙트마다 최대 128개의 값을 색, 켜기/끄기, 정수, 3D 벡터 종류로 넣고 접이식 그룹으로 묶을 수 있어요. 스튜디오에서는 값마다(색은 채널마다) 키를 찍고, 화면 효과 스택과 월드 이펙트는 프로젝트에 함께 저장돼요. 마음에 드는 조명·화면 효과·표면 셰이더는 "룩" 하나로 저장해 다른 장면에 그대로 적용할 수 있어요. 이펙트를 만드는 분은 댄서의 본과 박자·온셋 입력, 표면 셰이더의 노멀·발광·태그 출력, 화면 효과용 오브젝트 ID 이미지, 패스 히스토리와 밉 체인, MME 이펙트를 옮기는 안내를 활용할 수 있어요.
- **갤러리가 모두에게 열렸어요**: 웹사이트에서 GitHub이나 Discord로 로그인해 누구나 이펙트를 올릴 수 있어요. 검토를 마친 이펙트에는 파란 인증 배지가 붙고, 인증되지 않은 이펙트는 설치 전에 한 번 물어봐요. 다른 갤러리 주소도 직접 추가할 수 있고, 앱이 꺼질 정도의 이펙트는 자동으로 꺼지며 나중에 다시 켤 수 있어요.
- **스튜디오가 더 편해졌어요**: 값마다 스톱워치로 애니메이션을 켜고 이전·다음 키로 이동할 수 있으며, 타임라인 눈금자 우클릭으로 구간을 정하고 재생 중에는 화면이 자동으로 따라가요. OKLab 색 선택기가 생겼고, 포즈 탭에서 모프를 고르면 타임라인의 해당 줄로 이동하며, 조명도 스톱워치로 값을 키에 담아 애니메이션할 수 있어요. 장면·조명·월드 이펙트 추가 메뉴도 한곳에 모았어요.

### 🐞 수정 및 개선
- **월드 파티클이 화면 공간 반사에 비치던 문제를 고쳤어요**: 눈처럼 월드에 그려지는 입자가 반사와 SSAO에 나타나지 않아요.
- **스튜디오 슬라이더와 OKLab 색 선택기 배치를 고쳤어요**: 애니메이션 중에도 슬라이더가 줄 너비를 그대로 유지하고, OKLab 선택기 창이 넓어지지 않아요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- 공식 갤러리의 이펙트들이 이 버전에 맞게 새로워졌어요. 새로워진 이펙트를 쓰려면 2.4.0이 필요해요.

## English
### ✨ New
- **Shader packs are now "MMDX Effects"**: they come in four kinds — surface shaders, screen effects, world effects and looks — and the lobby tab is now "Effects".
- **World effects and the built-in "Snow" are here**: snow falls, settles into footprints where the dancers step, and kicks up at their feet. Turn it on from the screen effects panel in play mode, or place it in the studio as an entity and key each of its values.
- **Effect values are now much more adjustable**: each effect can have up to 128 values typed as colour, on/off, whole number or 3D vector, organised in collapsible groups. In the studio you can key each value (and each colour channel); screen-effect stacks and world entities are saved with the project. Save your favourite lighting, screen effects and surface shader as a single look and apply it elsewhere. If you make effects, you can use performer bones and beat / onset inputs, output normal / emission / tags from surface shaders, an object ID image for screen effects, pass history and mip chains, and a guide for porting MME effects.
- **The gallery is open to everyone**: sign in to the website with GitHub or Discord and publish your own effects. Reviewed packs get a blue verified badge, and unverified packs ask before installing. You can add other gallery addresses, and a pack that crashes the app is turned off automatically and can be turned on again.
- **The studio is easier to work in**: a stopwatch per value turns animation on with previous / next key navigation, right-clicking the timeline ruler sets a range, and the timeline follows playback. There is an OKLab colour picker, choosing a morph in the pose tab jumps to its timeline row, and lights can be animated with a stopwatch too. Adding to the scene, lights and world effects now lives in one menu.

### 🐞 Fixes & Improvements
- **Fixed world particles showing up in screen-space reflections**: particles drawn in the world, such as snow, no longer appear in reflections or SSAO.
- **Fixed studio slider and OKLab picker layout**: sliders keep the full row width while animated, and the OKLab picker no longer grows wider.

### 📦 Notes
- Available through the automatic update (only the changed files are downloaded).
- The official gallery's effects were updated for this version and need 2.4.0.

## 日本語
### ✨ 新機能
- **シェーダーパックが「MMDX エフェクト」になりました**: サーフェスシェーダー、画面エフェクト、ワールドエフェクト、ルックの 4 種類に分かれ、ロビーのタブ名も「エフェクト」になりました。
- **ワールドエフェクトと標準の「Snow」が加わりました**: 雪が降り、ダンサーの足が触れた場所には雪が積もって足跡が残り、足元から雪が跳ねます。プレイモードでは画面エフェクトパネルからオンにでき、スタジオではシーンに配置して値ごとにキーを打てます。
- **エフェクトの値を細かく調整できるようになりました**: エフェクトごとに最大 128 個の値を、色・オン/オフ・整数・3D ベクトルの種類で設定し、折りたたみグループにまとめられます。スタジオでは値ごと(色はチャンネルごと)にキーを打て、画面エフェクトのスタックとワールドエフェクトはプロジェクトに保存されます。お気に入りの照明・画面エフェクト・サーフェスシェーダーを 1 つの「ルック」として保存し、別のシーンに適用できます。エフェクトを作る方は、演者のボーンとビート・オンセット入力、サーフェスシェーダーの法線・発光・タグ出力、画面エフェクト用のオブジェクト ID 画像、パス履歴とミップチェーン、MME エフェクトの移植ガイドを利用できます。
- **ギャラリーが誰にでも開かれました**: ウェブサイトで GitHub または Discord にサインインすれば、誰でもエフェクトを公開できます。審査を終えたパックには青い認証バッジが付き、未認証のパックはインストール前に確認します。他のギャラリーのアドレスも追加でき、アプリが落ちるようなパックは自動的にオフになり、後から再びオンにできます。
- **スタジオが使いやすくなりました**: 値ごとのストップウォッチでアニメーションをオンにし、前後のキーへ移動できます。タイムラインのルーラーを右クリックすると範囲を指定でき、再生中は自動でスクロールします。OKLab カラーピッカーが加わり、ポーズタブでモーフを選ぶとタイムラインの該当行へ移動し、ライトもストップウォッチで値をキーに打ってアニメーションできます。シーン・ライト・ワールドエフェクトの追加メニューも 1 か所にまとめました。

### 🐞 修正と改善
- **ワールドパーティクルがスクリーンスペース反射に映る問題を修正しました**: 雪のようにワールドに描かれるパーティクルが、反射や SSAO に現れなくなりました。
- **スタジオのスライダーと OKLab ピッカーの配置を修正しました**: アニメーション中もスライダーが行の幅を保ち、OKLab ピッカーのウィンドウが広がらなくなりました。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみダウンロードします)。
- 公式ギャラリーのエフェクトがこのバージョン向けに更新されました。新しいエフェクトを使うには 2.4.0 が必要です。

## 简体中文
### ✨ 新功能
- **着色器包现在叫"MMDX 特效"**: 分为表面着色器、画面特效、世界特效、外观预设四类,大厅的标签也改为"特效"。
- **新增世界特效和内置的"Snow"**: 雪花飘落,舞者踩过的地方会积雪并留下脚印,脚下还会溅起雪。播放模式下可在画面特效面板中开启,工作室中可放入场景并为每个数值打关键帧。
- **特效数值可以细致调整了**: 每个特效最多可添加 128 个数值,类型包括颜色、开关、整数和 3D 向量,并可用可折叠分组整理。在工作室中可为每个数值(颜色的每个通道)打关键帧,画面特效栈和世界特效会随项目一起保存。喜欢的灯光、画面特效和表面着色器可以保存为一个"外观预设",直接应用到其他场景。制作特效的用户还可以使用舞者骨骼与节拍 / 起音输入、表面着色器的法线 / 发光 / 标签输出、供画面特效使用的对象 ID 图像、通道历史与 mip 链,以及 MME 特效移植指南。
- **画廊向所有人开放了**: 在网站用 GitHub 或 Discord 登录后,任何人都可以发布特效。通过审核的包会显示蓝色认证徽章,未认证的包在安装前会询问一次。也可以自行添加其他画廊地址,导致应用崩溃的包会自动关闭,之后可以重新开启。
- **工作室更顺手了**: 每个数值都有秒表开关,并可用上一个 / 下一个关键帧跳转;右键单击时间轴标尺可设置范围,播放时自动滚动。新增 OKLab 取色器,在姿态标签中选择表情会跳到时间轴对应行,灯光也能用秒表把数值记录为关键帧。场景、灯光和世界特效的添加菜单也集中到了一处。

### 🐞 修复与改进
- **修复了世界粒子出现在屏幕空间反射中的问题**: 雪花等绘制在世界中的粒子不再出现在反射和 SSAO 中。
- **修复了工作室滑块和 OKLab 取色器的布局**: 动画时滑块保持整行宽度,OKLab 取色器窗口不再变宽。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件)。
- 官方画廊的特效已针对此版本更新,使用更新后的特效需要 2.4.0。
