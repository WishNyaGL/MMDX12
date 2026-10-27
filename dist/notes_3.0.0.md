<img src="https://raw.githubusercontent.com/digital8150/MMDX12/master/docs/media/stages-3.0.0.jpg" alt="MMDX12 3.0.0 - MMDX Stages" width="100%">

# MMDX12 3.0.0 — MMDX Stages: The Stage Comes Alive

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어

### 무대가 배경에서 공연장으로
지금까지의 스테이지는 MMD 시절과 똑같이 "모델 파일 하나"였어요. 조명은 앱의 프리셋을 따랐고, 전광판은 그림이 고정된 텍스처였으며, 하늘은 두 가지 색의 그라디언트였어요.

3.0에서는 MMDX12 전용 스테이지 형식인 **MMDX 스테이지**가 새로 생겼어요. 이 형식의 스테이지는 **자신의 조명 리그, 전광판 영상, 하늘과 날씨, 안개, 거울, 전용 이펙트를 직접 가지고 다니는 공연장**이에요. 조명이 곡에 맞춰 색을 바꾸며 움직이고, 전광판에는 다른 카메라가 찍은 무대가 실시간으로 나오고, 그 화면의 빛이 캐릭터를 비추고, 안개 속에 스포트라이트의 빛줄기가 그려져요. 이 모든 요소가 래스터·레이 트레이싱·패스 트레이싱·오프라인 GI의 네 가지 렌더러에서 똑같이 동작해요.

이를 위해 렌더러의 조명 구조를 처음부터 다시 만들었어요. 한 장면에 넣을 수 있는 조명의 수, 조명을 계산하는 방식, 하늘과 안개를 다루는 방식이 모두 바뀌었어요.

### 🎪 MMDX 스테이지 (새 형식)
- **기존 MMD 무대도 지금처럼 그대로 쓸 수 있어요**: MMDX 스테이지는 MMD 무대를 대체하지 않고, 그 위에 추가된 MMDX12 전용 형식이에요.
- **`stage.json` 하나로 공연장을 정의해요**: 무대 모델, 조명 리그, 전광판, 하늘, 안개, 카메라, 이펙트를 폴더 하나에 담아요. 기존 PMX·glTF·FBX 모델을 그대로 부품으로 쓸 수 있어요.
- **음악에 맞춰 움직여요**: 조명의 색과 세기, 무빙 라이트의 방향, 재질의 발광이 곡의 시간에 맞춰 바뀌어요. 스테이지마다 여러 가지 연출(배리언트)을 넣어 둘 수 있어요.
- **스튜디오에서 하나의 개체로 다뤄요**: 스테이지 파라미터를 타임라인에서 키로 잡고, 스테이지에 들어 있는 큐를 키로 가져와서 직접 고칠 수 있어요. 수정한 내용은 프로젝트에만 저장되고, 설치된 스테이지는 그대로 남아요.
- **스테이지 갤러리**: 앱 상단의 "갤러리"에서 스테이지를 받아 바로 설치하고, 업데이트하고, 지울 수 있어요. 다운로드가 끊겨도 이어서 받아요.

### 💡 조명이 진짜 공연장처럼
- **조명 256개**: 한 장면에 조명을 최대 256개까지 둘 수 있어요. 그림자는 화면에 가장 크게 기여하는 스포트라이트 16개에 자동으로 배정돼요.
- **Forward+ 클러스터 컬링**: 화면을 작은 3차원 구역으로 나누고 각 구역에 닿는 조명만 계산해서, 조명이 많아져도 프레임 속도가 버텨요.
- **고보(Gobo)**: 스포트라이트가 무늬를 투사하고, 무늬가 회전해요. 안개 속의 빛줄기에도 같은 무늬가 나타나요.
- **전광판이 실제 광원이 돼요**: 이미지, 동영상, 다른 카메라의 실시간 화면, 셰이더 프로그램을 전광판에 띄울 수 있어요. 화면에 나오는 색이 주변의 캐릭터와 바닥을 비춰요.
- **발광하는 표면이 장면을 비춰요**: 패스 트레이싱과 오프라인 GI에서 LED, 네온, 발광 재질이 실제 조명으로 계산돼요.

### 🌫️ 하늘, 안개, 거울
- **물리 기반 하늘**: 시간대와 대기의 탁도를 정하면 낮부터 노을과 밤까지 하늘이 계산되고, 그 하늘이 장면 전체를 비춰요. HDRI와 Ray-MMD 스카이 돔 이미지도 하늘로 쓸 수 있어요.
- **구름**: 양, 높이, 크기, 바람을 정할 수 있고, 스튜디오에서 키로 움직일 수도 있어요.
- **실시간 볼류메트릭 안개**: 조명마다 빛줄기의 세기를 따로 정할 수 있어요. 스포트라이트의 빛줄기는 화면의 픽셀 단위로 계산해서 선명하게 그려져요. 오프라인 GI에서는 안개를 경로 추적으로 계산하기 때문에, 반사된 빛과 발광 표면까지 안개를 밝혀요.
- **평면 거울**: 무대의 거울 재질에 실시간 반사가 그려져요.
- **유리와 투명 재질**: glTF의 투과·굴절 재질이 패스 트레이싱과 GI에서 실제로 빛을 굴절시키고, 두께에 따라 색을 흡수해요.

### 🎬 스튜디오
- **타임라인 그래프**: 키의 값과 속도 그래프를 타임라인 안에서 바로 보고, 베지어 핸들을 끌어 보간을 고칠 수 있어요. 채널마다 그래프를 따로 열 수 있어요.
- **스톱워치와 키 이동**: After Effects처럼 각 행에서 애니메이션을 켜고, 이전·다음 키로 이동해요.
- **카메라 개체**: 메인 카메라 외에 카메라를 여러 대 두고, 그 화면을 무대 전광판에 띄울 수 있어요.
- **뷰포트 렌더러 선택**: 뷰포트를 레이 트레이싱, 패스 트레이싱, GI 미리보기로 바꿔 가며 작업할 수 있어요.

### ⚡ 성능
- **오프라인 GI 렌더가 더 빨라졌어요**: 1080p 스틸은 약 28 %, 영상은 프레임당 약 29 % 빨라졌어요.
- **압축 텍스처(DDS BCn)**: 압축된 텍스처를 풀지 않은 채로 GPU에 올려서 그래픽 메모리를 아껴요.

### 🌸 갤러리: 벚꽃
- **벚꽃(Sakura) 월드 이펙트를 공개해요**: 꽃잎이 흩날리다가 바닥에 쌓이고, 캐릭터의 머리와 어깨에도 내려앉아요. 무대 조명을 받으면 얇은 꽃잎에 빛이 비쳐요. 3.0.0 이상에서 쓸 수 있어요.

### 🐞 수정
- 조명이 많은 장면에서 캐릭터를 비추는 조명이 깜빡이던 문제를 고쳤어요.
- 스튜디오의 4분할 뷰에서 업스케일러를 켜면 화면이 뭉개지던 문제를 고쳤어요.
- 일부 glTF 모델에서 앰비언트 오클루전 맵이 없는 재질이 검게 나오던 문제를 고쳤어요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- 2.x에서 만든 스튜디오 프로젝트는 그대로 열려요. 3.0에서 저장한 프로젝트는 2.x에서 열 수 없어요.

## English

### From backdrop to concert venue
Until now a stage was what it was in MMD: one model file. Its lighting followed the app's presets, its LED walls were fixed textures, and its sky was a two-colour gradient.

3.0 adds **MMDX stages**, a stage format made for MMDX12. An MMDX stage is **a venue that carries its own lighting rig, LED wall content, sky and weather, haze, mirrors and effects**. Its lights move and change colour with the song, its walls show the stage live from other cameras, the light of those walls falls on the characters, and spotlight beams cut through the haze. All of it works the same in the four renderers: raster, ray tracing, path tracing and offline GI.

To get there, the renderer's lighting was rebuilt from the ground up: how many lights a scene can hold, how they are computed, and how sky and haze are handled have all changed.

### 🎪 MMDX stages (new format)
- **Your MMD stages keep working exactly as before**: MMDX stages do not replace them; they are an MMDX12-only format added on top.
- **One `stage.json` defines the venue**: the stage models, lighting rig, LED walls, sky, haze, cameras and effects live in one folder. Existing PMX / glTF / FBX models are used as parts as they are.
- **It moves with the music**: light colours and intensities, moving-head directions and material glow follow the song time. A stage can ship several looks (variants).
- **One entity in the Studio**: key the stage's parameters on the timeline, import its cues as keys and edit them. Your changes are saved in the project only; the installed stage stays untouched.
- **Stage gallery**: download, install, update and remove stages from "Gallery" at the top of the app. Interrupted downloads resume.

### 💡 Lighting like a real venue
- **256 lights**: a scene can hold up to 256 lights. Shadows go automatically to the 16 spotlights that matter most on screen.
- **Forward+ clustered culling**: the view is split into small 3D cells and each cell computes only the lights that reach it, so the frame rate holds as lights pile up.
- **Gobos**: spotlights project rotating patterns, and the same patterns show in their beams through the haze.
- **LED walls are real light sources**: show an image, a video, another camera's live view or a shader program on a wall, and its colours light the characters and the floor around it.
- **Glowing surfaces light the scene**: in path tracing and offline GI, LEDs, neon and emissive materials are computed as real lights.

### 🌫️ Sky, haze and mirrors
- **Physically based sky**: set the time of day and the turbidity, and the sky is computed from noon through sunset to night and lights the whole scene. HDRIs and Ray-MMD sky dome images work as skies too.
- **Clouds**: coverage, height, size and wind, all keyable in the Studio.
- **Real-time volumetric haze**: every light has its own beam strength, and spotlight beams are computed per pixel, so they stay sharp. Offline GI path-traces the haze, so bounced light and glowing surfaces light it as well.
- **Planar mirrors**: mirror materials on a stage show real-time reflections.
- **Glass and transmissive materials**: glTF transmission materials really refract light in path tracing and GI and absorb colour by thickness.

### 🎬 Studio
- **Timeline graphs**: see a key's value and speed graphs right in the timeline and drag Bezier handles to shape the interpolation. Each channel opens its own graph.
- **Stopwatches and key navigation**: like After Effects, turn animation on per row and jump to the previous / next key.
- **Camera entities**: add cameras beside the main camera and show their views on the stage's LED walls.
- **Viewport renderer**: switch the viewport between ray tracing, path tracing and the GI preview while you work.

### ⚡ Performance
- **Faster offline GI renders**: about 28 % faster for a 1080p still and about 29 % faster per video frame.
- **Compressed textures (DDS BCn)**: compressed textures go to the GPU without being unpacked, saving video memory.

### 🌸 Gallery: Sakura
- **The Sakura world effect is now public**: petals drift down, pile up on the floor and settle on the characters' heads and shoulders, and stage lights shine through the thin petals. Requires 3.0.0 or later.

### 🐞 Fixes
- Lights on the characters no longer flicker in scenes with many lights.
- The Studio's quad view no longer looks blurry with an upscaler on.
- Some glTF materials without an ambient occlusion map no longer render black.

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).
- Studio projects made in 2.x open as they are. Projects saved in 3.0 cannot be opened in 2.x.

## 日本語

### 背景からライブ会場へ
これまでのステージは、MMD の頃と同じく「モデルファイル 1 つ」でした。照明はアプリのプリセットに従い、LED スクリーンは絵の決まったテクスチャで、空は 2 色のグラデーションでした。

3.0 では MMDX12 専用のステージ形式 **MMDX ステージ** が加わりました。このステージは **自分の照明リグ、スクリーン映像、空と天気、霧、鏡、専用エフェクトを持ち運ぶライブ会場** です。照明は曲に合わせて色を変えながら動き、スクリーンには別のカメラが映すステージがリアルタイムで流れ、その光がキャラクターを照らし、霧の中にスポットライトの光芒が描かれます。これらはすべて、ラスター・レイトレーシング・パストレーシング・オフライン GI の 4 つのレンダラーで同じように動作します。

そのためにレンダラーの照明の仕組みを一から作り直しました。1 シーンに置けるライトの数、ライトの計算方法、空と霧の扱いがすべて変わりました。

### 🎪 MMDX ステージ（新形式）
- **これまでの MMD ステージもそのまま使えます**: MMDX ステージは MMD ステージを置き換えるものではなく、その上に追加された MMDX12 専用の形式です。
- **`stage.json` ひとつで会場を定義**: ステージモデル、照明リグ、スクリーン、空、霧、カメラ、エフェクトを 1 つのフォルダーにまとめます。既存の PMX・glTF・FBX モデルをそのままパーツとして使えます。
- **音楽に合わせて動きます**: ライトの色と強さ、ムービングライトの向き、マテリアルの発光が曲の時間に合わせて変わります。ステージごとに複数の演出（バリアント）を入れておけます。
- **スタジオでは 1 つのオブジェクトとして扱えます**: ステージのパラメーターをタイムラインでキーにし、ステージに入っているキューをキーとして取り込んで編集できます。変更はプロジェクトにだけ保存され、インストールしたステージはそのまま残ります。
- **ステージギャラリー**: アプリ上部の「ギャラリー」からステージをダウンロードして、そのままインストール・更新・削除できます。中断したダウンロードは続きから再開します。

### 💡 本物のライブ会場のような照明
- **ライト 256 個**: 1 シーンに最大 256 個のライトを置けます。影は画面への寄与が最も大きい 16 個のスポットライトに自動で割り当てられます。
- **Forward+ クラスターカリング**: 画面を小さな 3 次元の区画に分け、各区画に届くライトだけを計算するので、ライトが増えてもフレームレートが保たれます。
- **ゴボ（Gobo）**: スポットライトが模様を投影し、模様が回転します。霧の中の光芒にも同じ模様が現れます。
- **スクリーンが本物の光源に**: 画像、動画、別カメラのリアルタイム映像、シェーダープログラムをスクリーンに映せます。映った色が周りのキャラクターや床を照らします。
- **発光する面がシーンを照らします**: パストレーシングとオフライン GI では、LED、ネオン、発光マテリアルが本物のライトとして計算されます。

### 🌫️ 空、霧、鏡
- **物理ベースの空**: 時間帯と大気の濁りを決めると、昼から夕焼け、夜までの空が計算され、その空がシーン全体を照らします。HDRI や Ray-MMD のスカイドーム画像も空として使えます。
- **雲**: 量、高さ、大きさ、風を設定でき、スタジオでキーを打って動かすこともできます。
- **リアルタイムのボリューメトリック霧**: ライトごとに光芒の強さを個別に設定できます。スポットライトの光芒はピクセル単位で計算するので、くっきり描かれます。オフライン GI では霧をパストレースで計算するため、反射した光や発光面まで霧を照らします。
- **平面ミラー**: ステージのミラーマテリアルにリアルタイムの反射が描かれます。
- **ガラスと透過マテリアル**: glTF の透過・屈折マテリアルが、パストレーシングと GI で実際に光を屈折させ、厚みに応じて色を吸収します。

### 🎬 スタジオ
- **タイムライングラフ**: キーの値と速度のグラフをタイムラインの中で直接確認し、ベジェハンドルをドラッグして補間を調整できます。チャンネルごとにグラフを開けます。
- **ストップウォッチとキー移動**: After Effects のように、各行でアニメーションをオンにし、前後のキーへ移動できます。
- **カメラオブジェクト**: メインカメラ以外にカメラを複数置き、その映像をステージのスクリーンに映せます。
- **ビューポートのレンダラー選択**: ビューポートをレイトレーシング、パストレーシング、GI プレビューに切り替えながら作業できます。

### ⚡ パフォーマンス
- **オフライン GI レンダーが高速に**: 1080p の静止画は約 28 %、動画は 1 フレームあたり約 29 % 速くなりました。
- **圧縮テクスチャ（DDS BCn）**: 圧縮テクスチャを展開せずに GPU へ送り、ビデオメモリを節約します。

### 🌸 ギャラリー: 桜
- **桜（Sakura）ワールドエフェクトを公開しました**: 花びらが舞い散って床に積もり、キャラクターの頭や肩にも舞い降ります。ステージの照明が薄い花びらを透かします。3.0.0 以降で使えます。

### 🐞 修正
- ライトの多いシーンで、キャラクターを照らすライトがちらつく問題を修正しました。
- スタジオの 4 分割ビューでアップスケーラーをオンにすると画面がぼやける問題を修正しました。
- 一部の glTF モデルで、アンビエントオクルージョンマップのないマテリアルが黒く表示される問題を修正しました。

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルだけをダウンロードします）。
- 2.x で作ったスタジオプロジェクトはそのまま開けます。3.0 で保存したプロジェクトは 2.x では開けません。

## 简体中文

### 从背景到演出场馆
此前的舞台和 MMD 时代一样，只是“一个模型文件”。灯光跟随应用的预设，LED 屏幕是固定画面的贴图，天空是双色渐变。

3.0 新增了 MMDX12 专用的舞台格式 **MMDX 舞台**。这种舞台是 **自带灯光阵列、屏幕画面、天空与天气、雾、镜面和专属特效的演出场馆**。灯光随歌曲变换颜色并移动，屏幕上实时播放其他摄像机拍摄的舞台，屏幕的光照亮角色，聚光灯的光束穿过雾气。这一切在光栅、光线追踪、路径追踪和离线 GI 四种渲染器中表现一致。

为此，渲染器的光照架构从头重建：一个场景能容纳的灯光数量、灯光的计算方式、天空与雾的处理方式都已改变。

### 🎪 MMDX 舞台（新格式）
- **原有的 MMD 舞台仍可照常使用**: MMDX 舞台并不取代 MMD 舞台，而是在其之上新增的 MMDX12 专用格式。
- **一个 `stage.json` 定义整个场馆**: 舞台模型、灯光阵列、屏幕、天空、雾、摄像机和特效都放在一个文件夹中。现有的 PMX・glTF・FBX 模型可直接作为部件使用。
- **随音乐而动**: 灯光的颜色与强度、摇头灯的方向、材质的发光都随歌曲时间变化。每个舞台可以内置多种演出方案（变体）。
- **在工作室中作为一个对象处理**: 可以在时间轴上为舞台参数打关键帧，把舞台自带的提示（cue）导入为关键帧并修改。修改只保存在项目中，已安装的舞台保持不变。
- **舞台画廊**: 在应用顶部的“画廊”中下载舞台，即可安装、更新和删除。下载中断后可以继续。

### 💡 真正的演出级灯光
- **256 盏灯**: 一个场景最多可放置 256 盏灯。阴影会自动分配给对画面贡献最大的 16 盏聚光灯。
- **Forward+ 分簇剔除**: 将画面划分为小的三维区块，每个区块只计算照到它的灯光，灯光再多帧率也能保持。
- **图案片（Gobo）**: 聚光灯可以投射并旋转图案，雾中的光束也会出现相同的图案。
- **屏幕成为真正的光源**: 屏幕可以显示图片、视频、其他摄像机的实时画面或着色器程序，画面的颜色会照亮周围的角色和地面。
- **发光表面照亮场景**: 在路径追踪和离线 GI 中，LED、霓虹和发光材质会作为真正的光源计算。

### 🌫️ 天空、雾与镜面
- **基于物理的天空**: 设定时间和大气浑浊度，即可计算从白天到黄昏和夜晚的天空，并由天空照亮整个场景。HDRI 和 Ray-MMD 天空球图像也可以作为天空使用。
- **云**: 可设置云量、高度、大小和风，也可以在工作室中用关键帧让它们变化。
- **实时体积雾**: 每盏灯的光束强度可以单独设置。聚光灯的光束按像素计算，因此清晰锐利。离线 GI 用路径追踪计算雾，反射光和发光表面也会照亮雾气。
- **平面镜**: 舞台上的镜面材质会显示实时反射。
- **玻璃与透射材质**: glTF 的透射・折射材质在路径追踪和 GI 中真实地折射光线，并按厚度吸收颜色。

### 🎬 工作室
- **时间轴曲线图**: 直接在时间轴中查看关键帧的数值与速度曲线，拖动贝塞尔手柄调整插值。每个通道可以单独打开曲线图。
- **秒表与关键帧跳转**: 像 After Effects 一样，在每一行开启动画，并跳转到上一个或下一个关键帧。
- **摄像机对象**: 除主摄像机外可放置多台摄像机，并把它们的画面显示在舞台屏幕上。
- **视口渲染器选择**: 工作时可在光线追踪、路径追踪和 GI 预览之间切换视口。

### ⚡ 性能
- **离线 GI 渲染更快**: 1080p 静帧快约 28 %，视频每帧快约 29 %。
- **压缩纹理（DDS BCn）**: 压缩纹理无需解压即可上传到 GPU，节省显存。

### 🌸 画廊: 樱花
- **樱花（Sakura）世界特效正式公开**: 花瓣飘落并堆积在地面上，也会落在角色的头和肩上；舞台灯光会透过轻薄的花瓣。需要 3.0.0 或更高版本。

### 🐞 修复
- 修复了灯光较多的场景中照亮角色的灯光闪烁的问题。
- 修复了工作室四分视图在开启超分辨率时画面模糊的问题。
- 修复了部分 glTF 模型中没有环境光遮蔽贴图的材质显示为黑色的问题。

### 📦 说明
- 可通过自动更新获取（只下载有变化的文件）。
- 2.x 制作的工作室项目可以直接打开。在 3.0 中保存的项目无法在 2.x 中打开。
