<img src="https://raw.githubusercontent.com/digital8150/MMDX12/master/docs/media/studio-ai-2.2.0.jpg" alt="MMDX12 2.2.0 - AI directs the Studio over MCP" width="100%">

# MMDX12 2.2.0 — AI Studio Directors (MCP tools & agent skills)

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **AI가 스튜디오를 직접 이해하고 연출할 수 있는 MCP 도구가 생겼어요**: 별도 스크립트나 VMD 파싱 없이 AI가 MCP로 장면(모델·크기·조명), 캐릭터 모션(점프·회전·멈춤·이동·대형 변화), 화면 구도(누가 잘렸는지, 샷 크기), 음악(BPM·비트·다운비트·구간)을 바로 읽어요. 카메라 키 일괄 읽기/쓰기, 구도 계산기와 샷 목록(돌리·줌·스윙·크레인·팔로우·비트 강조), 조명 리그와 음악에 맞춘 조명 큐, 모션 레이어 편집도 제공해요. AI의 수정은 한 번의 실행 취소(Ctrl+Z)로 되돌릴 수 있어요.
- **AI가 결과를 눈으로 보고 고칠 수 있어요**: 원하는 프레임들을 실제 렌더러로 찍은 콘택트 시트, 입 모양 모프로 지금 노래하는 멤버를 찾아주는 가창자 감지, 모션의 딱딱한 구간(등속·급정지·굳은 관절)을 짚어주는 모션 품질 진단을 제공해요.
- **딱딱한 MMD 모션을 부드럽게 다듬는 편집이 추가됐어요**: 이징, 관절 오버랩(팔로우 스루), 스프링, 호흡을 기존 키 위에 레이어로 얹어, 안무는 그대로 두고 움직임만 사람답게 다듬어요.
- **카메라 감독·조명 감독·모션 편집 에이전트 스킬 3종이 배포 zip에 포함돼요**: `skills/` 폴더를 Claude Code, Gemini, Codex 등의 스킬 폴더에 넣으면, AI가 곡을 듣고 트리트먼트 → 스토리보드 → 촬영 → 콘택트 시트 검토 → 수정의 감독 루프로 작업해요. 설치 방법은 `skills/README.md`와 `MCP_GUIDE.md`를 보세요.

### 🐞 수정 및 개선
- **실시간 PT 영상 렌더의 패스 누적이 제대로 수렴해요**: 머리카락 아래 피부, 유리, 실루엣 가장자리에 남던 점묘 노이즈와 계단 현상이 패스를 늘릴수록 깨끗해지고, 윤곽선도 끊기지 않는 선으로 나와요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### ✨ New
- **AI can now understand and direct the Studio through new MCP tools**: without scripts or VMD parsing, an AI reads the scene (models, sizes, lights), character motion (jumps, turns, holds, travel, formation changes), framing (who is cut off, shot size) and the music (BPM, beats, downbeats, sections) over MCP. It also gets bulk camera key read/write, a framing solver and shot lists (dolly, zoom, swing, crane, follow, beat accents), lighting rigs with music-driven light cues, and layered motion edits. Every AI edit is a single undo step (Ctrl+Z).
- **AI can look at its own results and revise them**: contact sheets rendered through the real renderer, singer detection from the mouth morphs (who is singing each line), and a motion quality check that points out stiff ranges (constant speed, dead stops, rigid joints).
- **New edits that loosen up stiff MMD motion**: ease, joint overlap (follow-through), spring and breathing are layered over the existing keys, so the choreography stays and only the movement becomes more human.
- **Three agent skills ship in the zip: camera director, lighting director and motion editor**: copy the `skills/` folder into the skill folder of Claude Code, Gemini, Codex and others, and the AI works through a director's loop: listen, treatment, storyboard, shoot, contact-sheet review, revise. See `skills/README.md` and `MCP_GUIDE.md` for setup.

### 🐞 Fixes & Improvements
- **Real-time PT video renders now converge as passes accumulate**: the dithering and stair steps on skin under hair, glass and silhouette edges clean up with more passes, and outlines render as continuous lines.

### 📦 Notes
- Available through the automatic update (only the changed files are downloaded).

## 日本語
### ✨ 新機能
- **AI がスタジオを理解して演出できる MCP ツールを追加しました**: スクリプトや VMD の解析なしで、AI が MCP 経由でシーン (モデル・サイズ・照明)、キャラクターのモーション (ジャンプ・ターン・静止・移動・フォーメーション変化)、構図 (誰が見切れているか、ショットサイズ)、音楽 (BPM・ビート・ダウンビート・セクション) を直接読み取れます。カメラキーの一括読み書き、構図ソルバーとショットリスト (ドリー・ズーム・スウィング・クレーン・フォロー・ビート強調)、照明リグと音楽に合わせた照明キュー、モーションのレイヤー編集も使えます。AI の編集はすべて一回の元に戻す (Ctrl+Z) で戻せます。
- **AI が結果を自分の目で確認して修正できます**: 実際のレンダラーで撮るコンタクトシート、口モーフから今歌っているメンバーを見つける歌唱者検出、硬い区間 (等速・急停止・固まった関節) を指摘するモーション品質診断を追加しました。
- **硬い MMD モーションをなめらかにする編集を追加しました**: イージング、関節のオーバーラップ (フォロースルー)、スプリング、呼吸を既存キーの上にレイヤーとして重ね、振り付けはそのままに動きだけを人間らしく整えます。
- **カメラ監督・照明監督・モーション編集のエージェントスキル 3 種を配布 zip に同梱しました**: `skills/` フォルダーを Claude Code・Gemini・Codex などのスキルフォルダーに入れると、AI が曲を聴いてトリートメント → 絵コンテ → 撮影 → コンタクトシート確認 → 修正という監督のループで作業します。導入方法は `skills/README.md` と `MCP_GUIDE.md` を参照してください。

### 🐞 修正および改善
- **リアルタイム PT 動画レンダーのパス蓄積が正しく収束するようになりました**: 髪の下の肌、ガラス、シルエットの縁に残っていたディザ状のノイズや階段状のギザギザがパスを重ねるほどきれいになり、輪郭線も途切れない線で描かれます。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみダウンロードします)。

## 简体中文
### ✨ 新功能
- **新增 MCP 工具，让 AI 能够理解并执导工作室场景**: 无需脚本或解析 VMD，AI 可通过 MCP 直接读取场景 (模型、尺寸、灯光)、角色动作 (跳跃、转身、停顿、移动、队形变化)、构图 (谁被裁切、景别) 以及音乐 (BPM、节拍、强拍、段落)。同时提供镜头关键帧批量读写、构图求解器与镜头列表 (推拉、变焦、摇臂、升降、跟拍、节拍强调)、灯光组与随音乐变化的灯光提示，以及动作图层编辑。AI 的每次修改都可以一次撤销 (Ctrl+Z)。
- **AI 可以亲眼查看结果并加以修改**: 新增用真实渲染器拍摄的联系表、通过口型变形检测当前演唱成员的演唱者检测，以及指出僵硬片段 (匀速、急停、关节僵直) 的动作质量诊断。
- **新增让僵硬 MMD 动作变柔和的编辑**: 在现有关键帧之上以图层叠加缓动、关节重叠 (跟随动作)、弹簧和呼吸，保留编舞，只让动作更有人味。
- **安装包内附带摄影指导、灯光指导、动作编辑三种智能体技能**: 将 `skills/` 文件夹放入 Claude Code、Gemini、Codex 等的技能文件夹后，AI 会按照导演流程工作：听歌 → 构思 → 分镜 → 拍摄 → 联系表审阅 → 修改。安装方法请参阅 `skills/README.md` 和 `MCP_GUIDE.md`。

### 🐞 修复与改进
- **实时 PT 视频渲染的多遍累积现在能正确收敛**: 发丝下的皮肤、玻璃和轮廓边缘残留的抖动噪点与锯齿会随遍数增加而变得干净，描边也呈现为连续的线条。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件)。
