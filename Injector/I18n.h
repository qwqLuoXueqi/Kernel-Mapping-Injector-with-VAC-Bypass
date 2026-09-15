#pragma once
// ────────────────────────────────────────────────────────────────────────────
//  injector / i18n
//  Resolves the panel language, then answers every label lookup from a table.
//
//  @role     util                 @thread  any once Init has run
//  @touches  win32, advapi32, stl
// ────────────────────────────────────────────────────────────────────────────
//
// >  Init() must run before the first T(). Before it does, every lookup
//    answers in English and no detection happens.
// ?  a key absent from the table returns itself, so a half-finished
//    translation degrades to English instead of to an empty label.
// ~  one linear scan per lookup; lookups are one per drawn label.
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

namespace I18n {

enum UiLang { LANG_EN = 0, LANG_ZH_CN, LANG_ZH_TW, LANG_JA, LANG_RU, LANG_DE, LANG_FR, LANG_COUNT };

constexpr int LANG_AUTO = -1;   // sentinel stored in the settings file

struct TrEntry {
    const wchar_t* key;
    const wchar_t* zhCN; const wchar_t* zhTW; const wchar_t* ja;
    const wchar_t* ru;   const wchar_t* de;   const wchar_t* fr;
};

// ^  each language names itself, which is the convention for a language picker.
inline const wchar_t* LangName(int lang)
{
    switch (lang) {
    case LANG_EN:    return L"English";
    case LANG_ZH_CN: return L"简体中文";
    case LANG_ZH_TW: return L"繁體中文";
    case LANG_JA:    return L"日本語";
    case LANG_RU:    return L"Русский";
    case LANG_DE:    return L"Deutsch";
    case LANG_FR:    return L"Français";
    default:         return L"English";
    }
}

// ---------------------------------------------------------------
// ^  the key column is the English source text and call sites pass it verbatim:
//    T(L"key"). A null cell falls through to English.
// x  long sentences carry the two Chinese variants only, short labels carry all
//    seven. Filling the gaps is left to whoever needs them.
// ---------------------------------------------------------------
inline const TrEntry g_tbl[] = {
    // ---- top bar and navigation ----
    {L"Inject", L"注入", L"注入", L"インジェクト", L"Инжект", L"Injizieren", L"Injecter"},
    {L"Log", L"日志", L"日誌", L"ログ", L"Журнал", L"Protokoll", L"Journal"},
    {L"Settings", L"设置", L"設定", L"設定", L"Настройки", L"Einstellungen", L"Paramètres"},
    {L"About", L"关于", L"關於", L"情報", L"О программе", L"Über", L"À propos"},
    {L"Injector · Panel", L"注入器 · 面板版", L"注入器 · 面板版", L"インジェクター · パネル",
     L"Инжектор · Панель", L"Injector · Panel", L"Injecteur · Panneau"},
    {L"Idle", L"待机", L"待機", L"待機中", L"Ожидание", L"Bereit", L"En attente"},
    {L"Injecting", L"注入进行中", L"注入進行中", L"注入中", L"Инжект…", L"Injiziert…", L"Injection…"},
    {L"LUOXUEQI Injector", L"LUOXUEQI 注入器", L"LUOXUEQI 注入器", L"LUOXUEQI インジェクター",
     L"LUOXUEQI Инжектор", L"LUOXUEQI Injector", L"LUOXUEQI Injecteur"},
    {L"LUOXUEQI Injector · Panel", L"LUOXUEQI 注入器 · 面板版", L"LUOXUEQI 注入器 · 面板版",
     L"LUOXUEQI インジェクター · パネル", L"LUOXUEQI Инжектор · Панель",
     L"LUOXUEQI Injector · Panel", L"LUOXUEQI Injecteur · Panneau"},

    // ---- page titles and subtitles ----
    {L"Pick a DLL and set the options, then inject", L"选择 DLL 与注入参数，然后开始注入", L"選擇 DLL 與注入參數，然後開始注入", L"DLL を選んでオプションを設定し、注入します", L"Выберите DLL, задайте параметры и запустите инжект", L"DLL wählen, Optionen einstellen, dann injizieren", L"Choisissez une DLL, réglez les options, puis injectez"},
    {L"Run log", L"运行日志", L"執行日誌", L"実行ログ", L"Журнал", L"Protokoll", L"Journal"},
    {L"Every step is written here, and also to injector.log next to the program", L"每一步都会写在这里，同时也会落到程序目录的 injector.log", L"每一步都會寫在這裡，同時也會寫入程式目錄的 injector.log", L"すべての手順はここに表示され、プログラム隣の injector.log にも記録されます", L"Каждый шаг пишется здесь и в injector.log рядом с программой", L"Jeder Schritt steht hier und auch in injector.log neben dem Programm", L"Chaque étape est écrite ici et dans injector.log à côté du programme"},
    {L"Default options & file locations", L"默认参数与文件位置", L"預設參數與檔案位置", L"既定のオプションとファイル位置", L"Параметры по умолчанию и файлы", L"Standardoptionen & Dateipfade", L"Options par défaut et fichiers"},
    {L"Version · hotkeys · notes", L"版本 · 快捷键 · 说明", L"版本 · 快捷鍵 · 說明", L"バージョン · ホットキー · 説明", L"Версия · горячие клавиши · заметки", L"Version · Hotkeys · Hinweise", L"Version · raccourcis · notes"},

    // ---- inject page: target module card ----
    {L"Target DLL", L"目标 DLL", L"目標 DLL", L"対象 DLL", L"Целевая DLL", L"Ziel-DLL", L"DLL cible"},
    {L"Auto-lists .dll files in this folder; or pick one from elsewhere", L"自动列出程序目录里的 .dll；也可以从别处挑一个", L"自動列出程式目錄裡的 .dll；也可以從別處挑一個", L"このフォルダの .dll を自動一覧、別の場所からも選択できます", L"Показывает .dll в этой папке; можно выбрать и в другом месте", L"Listet .dll in diesem Ordner auf; oder anderswo wählen", L"Liste les .dll de ce dossier ; ou choisissez ailleurs"},
    {L"Folder: ", L"目录：", L"目錄：", L"フォルダ: ", L"Папка: ", L"Ordner: ", L"Dossier : "},
    {L"Browse...", L"浏览…", L"瀏覽…", L"参照…", L"Обзор…", L"Durchsuchen…", L"Parcourir…"},
    {L"Refresh", L"刷新", L"重新整理", L"更新", L"Обновить", L"Aktualisieren", L"Actualiser"},
    {L"Open folder", L"打开目录", L"開啟目錄", L"フォルダを開く", L"Открыть папку",
     L"Ordner öffnen", L"Ouvrir le dossier"},
    {L"(No .dll in this folder — click Browse to pick one)", L"（本目录没有 .dll，点『浏览…』选一个）", L"（本目錄沒有 .dll，點『瀏覽…』選一個）", L"（このフォルダに .dll がありません — 参照… で選択）", L"(Нет .dll в этой папке — нажмите «Обзор…»)", L"(Keine .dll in diesem Ordner — „Durchsuchen…“)", L"(Aucun .dll ici — cliquez sur « Parcourir… »)"},

    // ---- inject page: options card ----
    {L"Inject options", L"注入参数", L"注入參數", L"注入オプション", L"Параметры", L"Optionen", L"Options"},
    {L"Target process", L"目标进程", L"目標處理序", L"対象プロセス", L"Процесс", L"Prozess", L"Processus"},
    {L"Injection method", L"注入方式", L"注入方式", L"注入方式", L"Способ", L"Methode", L"Méthode"},
    {L"LoadLibrary (recommended)", L"LoadLibrary（推荐）", L"LoadLibrary（推薦）", L"LoadLibrary（推奨）",
     L"LoadLibrary (рекоменд.)", L"LoadLibrary (empfohlen)", L"LoadLibrary (recommandé)"},
    {L"Manual map", L"手动映射", L"手動映射", L"手動マップ", L"Ручной мэппинг", L"Manual Map", L"Mapping manuel"},
    {L"Manual map crashes DLLs with TLS/CRT — not recommended", L"手动映射对带 TLS/CRT 的 DLL 会崩，不建议", L"手動映射對帶 TLS/CRT 的 DLL 會崩，不建議", L"手動マップは TLS/CRT 付き DLL でクラッシュします（非推奨）", L"Ручной мэппинг роняет DLL с TLS/CRT — не рекомендуется", L"Manual Map stürzt DLLs mit TLS/CRT ab — nicht empfohlen", L"Le mapping manuel plante les DLL avec TLS/CRT — déconseillé"},
    {L"Wait for game", L"等待游戏启动", L"等待遊戲啟動", L"ゲーム起動を待つ", L"Ждать игру",
     L"Auf Spiel warten", L"Attendre le jeu"},
    {L"Open the panel first, then start the game", L"先开面板，再启动游戏", L"先開面板，再啟動遊戲", L"先にパネルを開いてから起動", L"Сначала панель, потом игра", L"Erst Panel, dann Spiel starten", L"Panneau d'abord, puis le jeu"},
    {L"Random instance", L"随机实例", L"隨機實例", L"ランダム実行", L"Случайный экземпляр",
     L"Zufällige Instanz", L"Instance aléatoire"},
    {L"Copy to a random name and run", L"复制成随机名再运行", L"複製成隨機名再執行", L"ランダム名にコピーして実行", L"Копировать под случайным именем и запустить", L"Unter zufälligem Namen kopieren und starten", L"Copier sous un nom aléatoire et lancer"},

    // ---- inject page: status card ----
    {L"Current status", L"当前状态", L"目前狀態", L"現在の状態", L"Состояние", L"Status", L"État"},
    {L" not running yet", L" 尚未启动", L" 尚未啟動", L" 未起動", L" не запущен", L" nicht gestartet", L" non démarré"},
    {L" ready to inject", L" 已就绪，可以注入", L" 已就緒，可以注入", L" 準備完了", L" готов", L" bereit", L" prêt"},
    {L"Injecting, please wait...", L"正在注入，请稍候…", L"正在注入，請稍候…", L"注入中、お待ちください…",
     L"Инжект, подождите…", L"Injiziert, bitte warten…", L"Injection, veuillez patienter…"},
    {L"Click Start Inject first, then launch the game; or turn off 'Wait for game' to inject right away", L"先点『开始注入』再启动游戏；也可以关掉『等待游戏启动』后直接注入", L"先點『開始注入』再啟動遊戲；也可以關掉『等待遊戲啟動』後直接注入", L"先に『注入開始』を押してからゲームを起動。『ゲーム起動を待つ』を切れば即注入も可", L"Сначала «Начать инжект», затем запустите игру; или выключите «Ждать игру»", L"Erst „Injizieren“, dann Spiel starten; oder „Auf Spiel warten“ ausschalten", L"Cliquez sur « Injecter » puis lancez le jeu ; ou désactivez « Attendre le jeu »"},

    // ---- footer status and buttons ----
    {L"Admin rights: enabled", L"管理员权限 已启用", L"管理員權限 已啟用", L"管理者権限: 有効",
     L"Права администратора: есть", L"Adminrechte: aktiv", L"Droits admin : activés"},
    {L"Not running as administrator", L"未以管理员运行", L"未以管理員執行", L"管理者として実行されていません",
     L"Запуск без прав администратора", L"Nicht als Administrator", L"Pas en administrateur"},
    {L" running", L" 运行中", L" 執行中", L" 実行中", L" запущен", L" läuft", L" en cours"},
    {L" not running", L" 未启动", L" 未啟動", L" 未起動", L" не запущен", L" nicht gestartet", L" arrêté"},
    {L"Waiting to start...", L"等待启动…", L"等待啟動…", L"起動待ち…", L"Ожидание запуска…",
     L"Warte auf Start…", L"En attente…"},
    {L"Start Inject", L"开始注入", L"開始注入", L"注入開始", L"Начать инжект", L"Injizieren", L"Injecter"},
    {L"Cancel", L"取消", L"取消", L"キャンセル", L"Отмена", L"Abbrechen", L"Annuler"},
    {L"Clear log", L"清空日志", L"清空日誌", L"ログを消去", L"Очистить журнал", L"Log leeren", L"Effacer le journal"},
    {L"Export to file", L"导出到文件", L"匯出到檔案", L"ファイルへ出力", L"Экспорт в файл",
     L"In Datei exportieren", L"Exporter"},

    // ---- settings page ----
    {L"Language", L"语言", L"語言", L"言語", L"Язык", L"Sprache", L"Langue"},
    {L"Auto (follow system)", L"自动（跟随系统）", L"自動（跟隨系統）", L"自動（システム）",
     L"Авто (по системе)", L"Auto (System)", L"Auto (système)"},
    {L"Defaults", L"默认参数", L"預設參數", L"既定値", L"По умолчанию", L"Standard", L"Par défaut"},
    {L"Stored in injector.ini and restored next time", L"这些会存进 injector.ini，下次打开自动带上", L"這些會存進 injector.ini，下次開啟自動帶上", L"injector.ini に保存され、次回起動時に復元されます", L"Сохраняется в injector.ini и восстанавливается при следующем запуске", L"Wird in injector.ini gespeichert und beim nächsten Start geladen", L"Enregistré dans injector.ini et restauré au prochain lancement"},
    {L"Files", L"文件", L"檔案", L"ファイル", L"Файлы", L"Dateien", L"Fichiers"},
    {L"Settings injector.ini        Log injector.log", L"设置 injector.ini        日志 injector.log", L"設定 injector.ini        日誌 injector.log", L"設定 injector.ini        ログ injector.log", L"Настройки injector.ini     Журнал injector.log", L"Einstellungen injector.ini    Log injector.log", L"Réglages injector.ini     Journal injector.log"},
    {L"Open ini", L"打开 ini", L"開啟 ini", L"ini を開く", L"Открыть ini", L"ini öffnen", L"Ouvrir ini"},
    {L"Admin rights: enabled (ready to inject)", L"管理员权限：已启用（可以注入）", L"管理員權限：已啟用（可以注入）", L"管理者権限：有効（注入可能）", L"Права администратора: есть (можно инжектить)", L"Adminrechte: aktiv (bereit zum Injizieren)", L"Droits admin : activés (prêt à injecter)"},
    {L"Admin rights: disabled — right-click and 'Run as administrator'", L"管理员权限：未启用 —— 请右键『以管理员身份运行』", L"管理員權限：未啟用 —— 請右鍵『以管理員身分執行』", L"管理者権限：無効 — 右クリックして「管理者として実行」", L"Права администратора: нет — ПКМ и «Запуск от имени администратора»", L"Adminrechte: inaktiv — Rechtsklick und „Als Administrator ausführen“", L"Droits admin : désactivés — clic droit et « Exécuter en tant qu'administrateur »"},
    {L"Panel build · 2026-09-14", L"面板版 · 2026-09-14", L"面板版 · 2026-09-14", L"パネル版 · 2026-09-14", L"Сборка панели · 2026-09-14", L"Panel-Build · 2026-09-14", L"Version panneau · 2026-09-14"},

    // ---- about page ----
    {L"Method: NtCreateThreadEx calls LoadLibraryW, falls back to LdrLoadDll", L"注入方式：NtCreateThreadEx 调 LoadLibraryW，失败自动回退 LdrLoadDll", L"注入方式：NtCreateThreadEx 呼叫 LoadLibraryW，失敗自動回退 LdrLoadDll", L"注入方式：NtCreateThreadEx が LoadLibraryW を呼び、失敗時は LdrLoadDll", L"Способ: NtCreateThreadEx вызывает LoadLibraryW, иначе LdrLoadDll", L"Methode: NtCreateThreadEx ruft LoadLibraryW, sonst LdrLoadDll", L"Méthode : NtCreateThreadEx appelle LoadLibraryW, sinon LdrLoadDll"},
    {L"Hook bypass: restores 12 hooked APIs before injecting, then puts them back", L"Hook 绕过：注入前恢复被 hook 的 12 个 API，注入后立刻还原", L"Hook 繞過：注入前恢復被 hook 的 12 個 API，注入後立刻還原", L"フック回避：注入前に 12 個のフック済み API を復元し、注入後に戻します", L"Обход хуков: восстанавливает 12 API перед инжектом, затем возвращает", L"Hook-Bypass: stellt 12 gehookte APIs vor dem Injizieren wieder her", L"Contournement : restaure 12 API hookées avant l'injection, puis les remet"},
    {L"Ready check: working set >= 600MB and stable over two samples", L"就绪判定：目标进程工作集 ≥600MB 且连续两次采样稳定", L"就緒判定：目標處理序工作集 ≥600MB 且連續兩次取樣穩定", L"準備判定：作業セット 600MB 以上かつ 2 回連続で安定", L"Проверка готовности: рабочий набор >= 600MB и стабилен два замера", L"Bereitschaft: Working Set >= 600MB und zwei stabile Messungen", L"Vérification : working set >= 600 Mo et stable sur deux mesures"},
    {L"Hotkeys: INSERT opens the menu in game / F4 unloads the DLL", L"快捷键：游戏内 INSERT 开菜单 / F4 卸载 DLL", L"快捷鍵：遊戲內 INSERT 開選單 / F4 卸載 DLL", L"ホットキー：ゲーム内 INSERT でメニュー / F4 で DLL をアンロード", L"Горячие клавиши: INSERT — меню в игре, F4 — выгрузить DLL", L"Hotkeys: INSERT öffnet das Menü im Spiel / F4 entlädt die DLL", L"Raccourcis : INSERT ouvre le menu en jeu / F4 décharge la DLL"},
    {L"Troubleshooting: injector.log next to the program records every step", L"排错：程序目录下的 injector.log 记录启动到注入的每一步", L"排錯：程式目錄下的 injector.log 記錄啟動到注入的每一步", L"トラブル時：プログラム隣の injector.log に全手順が記録されます", L"Диагностика: injector.log рядом с программой пишет все шаги", L"Fehlersuche: injector.log neben dem Programm protokolliert alles", L"Dépannage : injector.log à côté du programme enregistre tout"},

    // ---- panel log ----
    {L"Program folder: ", L"程序目录：", L"程式目錄：", nullptr, nullptr, nullptr, nullptr},
    {L"Found ", L"检测到 ", L"偵測到 ", nullptr, nullptr, nullptr, nullptr},
    {L" DLL file(s)", L" 个 DLL", L" 個 DLL", nullptr, nullptr, nullptr, nullptr},
    {L"Usage: pick a DLL -> Start Inject -> launch CS2 (or turn off 'Wait for game')", L"用法：选 DLL → 开始注入 → 启动 CS2（或关掉『等待游戏启动』直接注入）", L"用法：選 DLL → 開始注入 → 啟動 CS2（或關掉『等待遊戲啟動』直接注入）", L"使い方：DLL を選ぶ → 注入開始 → CS2 を起動（『ゲーム起動を待つ』を切れば即注入）", L"Порядок: выбрать DLL -> Начать инжект -> запустить CS2 (или выключить «Ждать игру»)", L"Ablauf: DLL wählen -> Injizieren -> CS2 starten (oder „Auf Spiel warten“ aus)", L"Ordre : choisir la DLL -> Injecter -> lancer CS2 (ou désactiver « Attendre le jeu »)"},
    {L"Select the DLL to inject", L"选择要注入的 DLL", L"選擇要注入的 DLL", nullptr, nullptr, nullptr, nullptr},
    {L"DLL files (*.dll)", L"DLL 文件 (*.dll)\0*.dll\0所有文件 (*.*)\0*.*\0", L"DLL 檔案 (*.dll)\0*.dll\0所有檔案 (*.*)\0*.*\0", nullptr, nullptr, nullptr, nullptr},
    {L"All files (*.*)", L"所有文件 (*.*)", L"所有檔案 (*.*)", nullptr, nullptr, nullptr, nullptr},
    {L"Refreshed, found ", L"已刷新，检测到 ", L"已重新整理，偵測到 ", nullptr, nullptr, nullptr, nullptr},
    {L"Selected: ", L"已选择：", L"已選擇：", nullptr, nullptr, nullptr, nullptr},
    {L"Log exported: ", L"日志已导出：", L"日誌已匯出：", nullptr, nullptr, nullptr, nullptr},
    {L"No DLL selected — cannot inject.", L"没有选择 DLL，无法注入。", L"沒有選擇 DLL，無法注入。", nullptr, nullptr, nullptr, nullptr},
    {L"Please pick a DLL to inject first.", L"请先选择要注入的 DLL。", L"請先選擇要注入的 DLL。", nullptr, nullptr, nullptr, nullptr},
    {L"Injection task started", L"开始注入任务", L"開始注入任務", nullptr, nullptr, nullptr, nullptr},
    {L"Cancel requested (the wait loop exits on the next poll)",
     L"已请求取消（等待循环会在下一次轮询时退出）",
     L"已請求取消（等待迴圈會在下一次輪詢時結束）", nullptr, nullptr, nullptr, nullptr},
    {L"Task finished: success", L"任务结束：注入成功", L"任務結束：注入成功", nullptr, nullptr, nullptr, nullptr},
    {L"Task finished: cancelled", L"任务结束：已取消", L"任務結束：已取消", nullptr, nullptr, nullptr, nullptr},
    {L"Task finished: inject failed", L"任务结束：注入失败", L"任務結束：注入失敗", nullptr, nullptr, nullptr, nullptr},
    {L"Task finished: target process not found", L"任务结束：未找到目标进程", L"任務結束：未找到目標處理序", nullptr, nullptr, nullptr, nullptr},
    {L"Task finished: invalid DLL", L"任务结束：DLL 无效", L"任務結束：DLL 無效", nullptr, nullptr, nullptr, nullptr},
    {L"Scroll with the wheel", L"滚轮可滚动", L"滾輪可捲動", nullptr, nullptr, nullptr, nullptr},

    // ---- driver log, produced by Main.cpp ----
    {L"Target process: ", L"目标进程： ", L"目標處理序： ", nullptr, nullptr, nullptr, nullptr},
    {L"Injection method: ", L"注入方式： ", L"注入方式： ", nullptr, nullptr, nullptr, nullptr},
    {L"Target DLL: ", L"目标 DLL： ", L"目標 DLL： ", nullptr, nullptr, nullptr, nullptr},
    {L"Manual map (this DLL will crash, not recommended)",
     L"手动映射（本 DLL 会崩，不建议）", L"手動映射（本 DLL 會崩，不建議）", nullptr, nullptr, nullptr, nullptr},
    {L"No DLL selected.", L"没有选择 DLL。", L"沒有選擇 DLL。", nullptr, nullptr, nullptr, nullptr},
    {L"DLL file not found: ", L"找不到 DLL 文件：", L"找不到 DLL 檔案：", nullptr, nullptr, nullptr, nullptr},
    {L"Target process not found: ", L"未能找到目标进程 ", L"未能找到目標處理序 ", nullptr, nullptr, nullptr, nullptr},
    {L"Failed to read DLL or file is empty; manual map aborted.",
     L"DLL 读取失败或文件为空，无法进行手动映射。", L"DLL 讀取失敗或檔案為空，無法進行手動映射。", nullptr, nullptr, nullptr, nullptr},
    {L"Manual map injection started...", L"开始手动映射注入...", L"開始手動映射注入...", nullptr, nullptr, nullptr, nullptr},
    {L"DLL memory buffer wiped.", L"DLL 内存缓冲区已安全擦除。", L"DLL 記憶體緩衝區已安全擦除。", nullptr, nullptr, nullptr, nullptr},
    {L"Restored ", L"已恢复 ", L"已恢復 ", nullptr, nullptr, nullptr, nullptr},
    {L" hooked API(s) (put back after injecting)", L" 个被 hook 的 API（注入后还原）",
     L" 個被 hook 的 API（注入後還原）", nullptr, nullptr, nullptr, nullptr},
    {L"Injecting...", L"正在注入...", L"正在注入...", nullptr, nullptr, nullptr, nullptr},
    {L"LoadLibraryW failed: ", L"LoadLibraryW 失败：", L"LoadLibraryW 失敗：", nullptr, nullptr, nullptr, nullptr},
    {L"LoadLibraryW failed, retrying with LdrLoadDll...",
     L"LoadLibraryW 失败，改用 LdrLoadDll 重试...", L"LoadLibraryW 失敗，改用 LdrLoadDll 重試...", nullptr, nullptr, nullptr, nullptr},
    {L", retrying with LdrLoadDll...", L"，改用 LdrLoadDll 重试...", L"，改用 LdrLoadDll 重試...", nullptr, nullptr, nullptr, nullptr},
    {L"LdrLoadDll failed: ", L"LdrLoadDll 失败：", L"LdrLoadDll 失敗：", nullptr, nullptr, nullptr, nullptr},
    {L"Hooks restored.", L"hook 已还原。", L"hook 已還原。", nullptr, nullptr, nullptr, nullptr},
    {L"Injected successfully! Bypassed ", L"注入成功！已绕过 ", L"注入成功！已繞過 ", nullptr, nullptr, nullptr, nullptr},
    {L" API(s)   (press INSERT in game for the menu)", L" 个 API   (游戏内按 INSERT 开菜单)",
     L" 個 API   (遊戲內按 INSERT 開選單)", nullptr, nullptr, nullptr, nullptr},
    {L"Injection failed.", L"注入失败。", L"注入失敗。", nullptr, nullptr, nullptr, nullptr},
    {L"Press any key to exit...", L"按任意键退出...", L"按任意鍵退出...", nullptr, nullptr, nullptr, nullptr},
    {L"No .dll found under: ", L"未在目录下找到任何 .dll 文件：", L"未在目錄下找到任何 .dll 檔案：", nullptr, nullptr, nullptr, nullptr},
    {L"Multiple DLLs found, console mode picks the first: ", L"目录下有多个 DLL，控制台模式默认选第一个：",
     L"目錄下有多個 DLL，主控台模式預設選第一個：", nullptr, nullptr, nullptr, nullptr},

    // ---- fragments assembled at runtime ----
    {L"Working set ", L"工作集 ", L"工作集 ", nullptr, nullptr, nullptr, nullptr},
    {L" MB     Threads ", L" MB    线程 ", L" MB    執行緒 ", nullptr, nullptr, nullptr, nullptr},
    {L"    Ready threshold 600 MB", L"    就绪阈值 600 MB", L"    就緒閾值 600 MB", nullptr, nullptr, nullptr, nullptr},
    {L": not running", L"：未启动", L"：未啟動", nullptr, nullptr, nullptr, nullptr},
    {L": running", L"：运行中", L"：執行中", nullptr, nullptr, nullptr, nullptr},

    // ---- modal dialogs ----
    {L"Failed to load Msftedit.dll, cannot start the panel.",
     L"加载 Msftedit.dll 失败，无法启动面板。", L"載入 Msftedit.dll 失敗，無法啟動面板。", nullptr, nullptr, nullptr, nullptr},
    {L"GDI+ initialization failed.", L"GDI+ 初始化失败。", L"GDI+ 初始化失敗。", nullptr, nullptr, nullptr, nullptr},

    // ---- secondary copy ----
    {L"Every step is written here", L"注入过程的每一步都会写在这里", L"注入過程的每一步都會寫在這裡", nullptr, nullptr, nullptr, nullptr},
};

// ─── language detection ──────────────────────────────────────────────────────
inline std::string RegStrA(HKEY root, const char* sub, const char* val)
{
    std::string out;
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (!adv) adv = LoadLibraryA("advapi32.dll");
    if (!adv) return out;
    typedef LONG(WINAPI* PFN_RGV)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    auto fn = (PFN_RGV)GetProcAddress(adv, "RegGetValueA");
    if (!fn) return out;
    char buf[512] = {};
    DWORD sz = sizeof(buf);
    if (fn(root, sub, val, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        out = buf;
    return out;
}

inline int MapSteamLang(const char* s)
{
    if (!s || !*s) return LANG_EN;
    if (!_stricmp(s, "schinese")) return LANG_ZH_CN;
    if (!_stricmp(s, "tchinese")) return LANG_ZH_TW;
    if (!_stricmp(s, "japanese")) return LANG_JA;
    if (!_stricmp(s, "russian"))  return LANG_RU;
    if (!_stricmp(s, "german"))   return LANG_DE;
    if (!_stricmp(s, "french"))   return LANG_FR;
    return LANG_EN;
}

inline bool ReadFileAll(const std::string& path, std::string& out)
{
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

// ?  an unrecognised locale falls back to English rather than to a guess.
inline int DetectSystemLang()
{
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::wstring s = loc;
        for (auto& c : s) c = (wchar_t)towlower(c);
        if (s.rfind(L"zh", 0) == 0) {
            if (s.find(L"hant") != std::wstring::npos || s.find(L"tw") != std::wstring::npos ||
                s.find(L"hk") != std::wstring::npos || s.find(L"mo") != std::wstring::npos)
                return LANG_ZH_TW;
            return LANG_ZH_CN;      // zh-CN, zh-Hans, zh-SG
        }
        if (s.rfind(L"ja", 0) == 0) return LANG_JA;
        if (s.rfind(L"ru", 0) == 0) return LANG_RU;
        if (s.rfind(L"de", 0) == 0) return LANG_DE;
        if (s.rfind(L"fr", 0) == 0) return LANG_FR;
        return LANG_EN;
    }
    // x  no locale name available — the primary language id is all this path has.
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_CHINESE:  return (SUBLANGID(GetUserDefaultUILanguage()) == SUBLANG_CHINESE_TRADITIONAL ||
                                SUBLANGID(GetUserDefaultUILanguage()) == SUBLANG_CHINESE_HONGKONG ||
                                SUBLANGID(GetUserDefaultUILanguage()) == SUBLANG_CHINESE_MACAU)
                               ? LANG_ZH_TW : LANG_ZH_CN;
    case LANG_JAPANESE: return LANG_JA;
    case LANG_RUSSIAN:  return LANG_RU;
    case LANG_GERMAN:   return LANG_DE;
    case LANG_FRENCH:   return LANG_FR;
    default:            return LANG_EN;
    }
}

// >  order is load-bearing. The per-game value outranks the Steam-wide one,
//    which outranks the operating system: someone who set the game to English
//    on a Chinese desktop expects an English panel.
inline int DetectLanguage()
{
    const std::string steam = [] {
        std::string s = RegStrA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
        if (s.empty())
            s = RegStrA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
        return s;
    }();

    if (!steam.empty()) {
        // >  every library folder, not only the default one.
        std::vector<std::string> libs;
        libs.push_back(steam);
        {
            std::string vdf;
            if (ReadFileAll(steam + "/steamapps/libraryfolders.vdf", vdf)) {
                const std::string key = "\"path\"";
                size_t pos = 0;
                while ((pos = vdf.find(key, pos)) != std::string::npos) {
                    const size_t a = vdf.find('"', pos + key.size());
                    if (a == std::string::npos) break;
                    const size_t b = vdf.find('"', a + 1);
                    if (b == std::string::npos) break;
                    libs.push_back(vdf.substr(a + 1, b - a - 1));
                    pos = b;
                }
            }
        }

        // !  the per-game manifest is authoritative for this title.
        // $  730 is the store id this tool targets; the file is written by Steam.
        for (const std::string& lib : libs) {
            std::string acf;
            if (!ReadFileAll(lib + "/steamapps/appmanifest_730.acf", acf)) continue;
            const size_t p = acf.find("\"language\"");
            if (p == std::string::npos) continue;
            const size_t a = acf.find('"', p + 10);
            if (a == std::string::npos) continue;
            const size_t b = acf.find('"', a + 1);
            if (b == std::string::npos) continue;
            const std::string lang = acf.substr(a + 1, b - a - 1);
            if (!lang.empty()) return MapSteamLang(lang.c_str());
        }

        // >  consulted only after every library folder has been read.
        const std::string global = RegStrA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "Language");
        if (!global.empty()) return MapSteamLang(global.c_str());
    }

    return DetectSystemLang();
}

// ─── lookup ──────────────────────────────────────────────────────────────────
inline int g_lang = LANG_EN;

inline void Set(int lang)
{
    g_lang = (lang >= 0 && lang < LANG_COUNT) ? lang : LANG_EN;
}

// ?  any out-of-range cfgLang means automatic, not English.
inline int Init(int cfgLang)
{
    Set(cfgLang >= 0 && cfgLang < LANG_COUNT ? cfgLang : DetectLanguage());
    return g_lang;
}

inline const wchar_t* T(const wchar_t* key)
{
    if (!key || !*key) return L"";
    if (g_lang == LANG_EN) return key;
    for (const TrEntry& e : g_tbl) {
        if (wcscmp(e.key, key) != 0) continue;
        const wchar_t* v = nullptr;
        switch (g_lang) {
        case LANG_ZH_CN: v = e.zhCN; break;
        case LANG_ZH_TW: v = e.zhTW; break;
        case LANG_JA:    v = e.ja;   break;
        case LANG_RU:    v = e.ru;   break;
        case LANG_DE:    v = e.de;   break;
        case LANG_FR:    v = e.fr;   break;
        default:         return key;
        }
        return (v && *v) ? v : key;   // row not translated -> English
    }
    return key;
}

// >  use this when the result is concatenated or must outlive the call.
inline std::wstring TW(const wchar_t* key) { return std::wstring(T(key)); }

} // namespace I18n
