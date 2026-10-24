#include "gui/uilanguage.h"

#include <QApplication>
#include <QColor>
#include <QHash>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QSettings>
#include <QTranslator>

#include <cmath>

namespace {

const auto LanguageKey = QStringLiteral("appearance/language");
const auto English = QStringLiteral("en");
const auto Norwegian = QStringLiteral("nb");
const auto SimplifiedChinese = QStringLiteral("zh_CN");
const auto TraditionalChinese = QStringLiteral("zh_TW");

// One row per English source text: Norwegian Bokmål, Simplified Chinese,
// Traditional Chinese. Keys are matched exactly, so a source text changed in
// the code must be changed here too.
struct TranslationRow {
  const char *source;
  const char *norwegian;
  const char *simplifiedChinese;
  const char *traditionalChinese;
};

const TranslationRow Translations[] = {
    // Main window
    {"No LANTalk hosts found", "Fant ingen LANTalk-verter", "未找到 LANTalk 主机", "找不到 LANTalk 主機"},
    {"File", "Fil", "文件", "檔案"},
    {"Exit LANTalk", "Avslutt LANTalk", "退出 LANTalk", "結束 LANTalk"},
    {"Connect...", "Koble til...", "连接...", "連線..."},
    {"Settings", "Innstillinger", "设置", "設定"},
    {"Font Size", "Skriftstørrelse", "字体大小", "字型大小"},
    {"General...", "Generelt...", "常规...", "一般..."},
    {"Connected to central host %1", "Tilkoblet sentral vert %1", "已连接到中心主机 %1", "已連線到中央主機 %1"},
    {"Not connected to central host %1: %2", "Ikke tilkoblet sentral vert %1: %2", "未连接到中心主机 %1：%2", "未連線到中央主機 %1：%2"},
    {"Connecting to central host %1…", "Kobler til sentral vert %1…", "正在连接到中心主机 %1…", "正在連線到中央主機 %1…"},
    {"Font size (pt):", "Skriftstørrelse (pt):", "字体大小（磅）：", "字型大小（點）："},
    {"Open LANTalk", "Åpne LANTalk", "打开 LANTalk", "開啟 LANTalk"},
    {"Quit", "Avslutt", "退出", "結束"},
    {"LANTalk — 1 unread message", "LANTalk — 1 ulest melding", "LANTalk — 1 条未读消息", "LANTalk — 1 則未讀訊息"},
    {"LANTalk — %1 unread messages", "LANTalk — %1 uleste meldinger", "LANTalk — %1 条未读消息", "LANTalk — %1 則未讀訊息"},
    {"Connect To", "Koble til", "连接到", "連線到"},
    {"Host IP or name:", "Vertens IP eller navn:", "主机 IP 或名称：", "主機 IP 或名稱："},
    {"Welcome to LANTalk", "Velkommen til LANTalk", "欢迎使用 LANTalk", "歡迎使用 LANTalk"},
    {"Choose a username:", "Velg et brukernavn:", "选择一个用户名：", "選擇一個使用者名稱："},
    {"Already connected to %1; switched to its tab.", "Allerede tilkoblet %1; byttet til fanen.", "已连接到 %1；已切换到其标签页。", "已連線到 %1；已切換到其分頁。"},
    {"A connection to %1 is already being established.", "En tilkobling til %1 er allerede på vei.", "正在建立与 %1 的连接。", "正在建立與 %1 的連線。"},
    {"Connecting to %1…", "Kobler til %1…", "正在连接到 %1…", "正在連線到 %1…"},
    {"Cannot listen on port %1: %2", "Kan ikke lytte på port %1: %2", "无法监听端口 %1：%2", "無法監聽連接埠 %1：%2"},
    {"Listening on TCP port %1", "Lytter på TCP-port %1", "正在监听 TCP 端口 %1", "正在監聽 TCP 連接埠 %1"},
    {"LAN discovery unavailable: %1", "LAN-søk er utilgjengelig: %1", "局域网发现不可用：%1", "區域網路探索無法使用：%1"},
    {"Scanning for hosts...", "Søker etter verter...", "正在扫描主机...", "正在掃描主機..."},
    {"Connecting to the central host...", "Kobler til den sentrale verten...", "正在连接到中心主机...", "正在連線到中央主機..."},
    {"Hosts", "Verter", "主机", "主機"},
    {"Online", "Pålogget", "在线", "在線上"},
    {"Away", "Borte", "离开", "離開"},
    {"Offline", "Frakoblet", "离线", "離線"},
    {"Web", "Web", "网页", "網頁"},
    {"End lantalk.exe in Task Manager", "Avslutt lantalk.exe i Oppgavebehandling", "在任务管理器中结束 lantalk.exe", "在工作管理員中結束 lantalk.exe"},
    {"Stop the lantalk process", "Stopp lantalk-prosessen", "停止 lantalk 进程", "停止 lantalk 處理程序"},
    {"LANTalk is already running but does not respond. %1 and start LANTalk again.", "LANTalk kjører allerede, men svarer ikke. %1 og start LANTalk på nytt.", "LANTalk 已在运行但没有响应。请%1，然后重新启动 LANTalk。", "LANTalk 已在執行但沒有回應。請%1，然後重新啟動 LANTalk。"},

    // Settings and Live Translate dialogs
    {"Live Translate", "Direkteoversettelse", "实时翻译", "即時翻譯"},
    {"Rescan", "Søk på nytt", "重新扫描", "重新掃描"},
    {"The central host translates for you.", "Den sentrale verten oversetter for deg.", "中心主机会为你翻译。", "中央主機會為你翻譯。"},
    {"Ollama server", "Ollama-server", "Ollama 服务器", "Ollama 伺服器"},
    {"Model", "Modell", "模型", "模型"},
    {"My Language", "Mitt språk", "我的语言", "我的語言"},
    {"Peer Language", "Motpartens språk", "对方的语言", "對方的語言"},
    {"Scanning the network for Ollama…", "Søker etter Ollama på nettverket…", "正在网络中搜索 Ollama…", "正在網路中搜尋 Ollama…"},
    {"No Ollama server found on the network.", "Fant ingen Ollama-server på nettverket.", "网络中未找到 Ollama 服务器。", "網路中找不到 Ollama 伺服器。"},
    {"1 Ollama server found on the network.", "Fant 1 Ollama-server på nettverket.", "在网络中找到 1 个 Ollama 服务器。", "在網路中找到 1 個 Ollama 伺服器。"},
    {"%1 Ollama servers found on the network.", "Fant %1 Ollama-servere på nettverket.", "在网络中找到 %1 个 Ollama 服务器。", "在網路中找到 %1 個 Ollama 伺服器。"},
    {"Nickname", "Kallenavn", "昵称", "暱稱"},
    {"Listen port", "Lytteport", "监听端口", "監聽連接埠"},
    {"None (chat directly on the LAN)", "Ingen (chat direkte på lokalnettet)", "无（直接在局域网中聊天）", "無（直接在區域網路中聊天）"},
    {"Central host", "Sentral vert", "中心主机", "中央主機"},
    {"None", "Ingen", "无", "無"},
    {"The password the central host was started with, if any.", "Passordet den sentrale verten ble startet med, om noe.", "中心主机启动时设置的密码（如有）。", "中央主機啟動時設定的密碼（如有）。"},
    {"Central host password", "Passord for sentral vert", "中心主机密码", "中央主機密碼"},
    {"Notify me about new messages", "Varsle meg om nye meldinger", "有新消息时通知我", "有新訊息時通知我"},
    {"Notifications", "Varsler", "通知", "通知"},
    {"Play a sound for new messages", "Spill av en lyd ved nye meldinger", "有新消息时播放提示音", "有新訊息時播放提示音"},
    {"Ollama URL", "Ollama-URL", "Ollama 地址", "Ollama 網址"},
    {"Ollama model", "Ollama-modell", "Ollama 模型", "Ollama 模型"},
    {"Language", "Språk", "语言", "語言"},
    {"LANTalk will use the new language the next time it starts.", "LANTalk bruker det nye språket neste gang programmet starter.", "LANTalk 将在下次启动时使用新语言。", "LANTalk 將在下次啟動時使用新語言。"},

    // Chat tab
    {"Close", "Lukk", "关闭", "關閉"},
    {"Performing nickname handshake…", "Utveksler kallenavn…", "正在交换昵称…", "正在交換暱稱…"},
    {"Message (Shift+Enter for a new line)", "Melding (Shift+Enter for ny linje)", "消息（Shift+Enter 换行）", "訊息（Shift+Enter 換行）"},
    {"Attach file", "Legg ved fil", "附加文件", "附加檔案"},
    {"Open file", "Åpne fil", "打开文件", "開啟檔案"},
    {"Live Translate is off.", "Direkteoversettelse er av.", "实时翻译已关闭。", "即時翻譯已關閉。"},
    {"Resuming Live Translate…", "Gjenopptar direkteoversettelse…", "正在恢复实时翻译…", "正在恢復即時翻譯…"},
    {"Connected to %1", "Tilkoblet %1", "已连接到 %1", "已連線到 %1"},
    {"Sent a file: %1", "Sendte en fil: %1", "发送了文件：%1", "傳送了檔案：%1"},
    {"Uploading %1 to %2. %3%…", "Laster opp %1 til %2. %3%…", "正在向 %2 上传 %1。%3%…", "正在向 %2 上傳 %1。%3%…"},
    {"Receiving %1 from %2. %3%…", "Mottar %1 fra %2. %3%…", "正在接收来自 %2 的 %1。%3%…", "正在接收來自 %2 的 %1。%3%…"},
    {"Could not send %1 to %2: %3", "Kunne ikke sende %1 til %2: %3", "无法将 %1 发送给 %2：%3", "無法將 %1 傳送給 %2：%3"},
    {"Did not receive %1 from %2: %3", "Mottok ikke %1 fra %2: %3", "未能接收来自 %2 的 %1：%3", "未能接收來自 %2 的 %1：%3"},
    {"Translating message...", "Oversetter melding...", "正在翻译消息...", "正在翻譯訊息..."},
    {"Live Translate: %1 ↔ %2", "Direkteoversettelse: %1 ↔ %2", "实时翻译：%1 ↔ %2", "即時翻譯：%1 ↔ %2"},
    {"Live Translate was not resumed: %1", "Direkteoversettelse ble ikke gjenopptatt: %1", "未能恢复实时翻译：%1", "未能恢復即時翻譯：%1"},
    {"Live Translate unavailable", "Direkteoversettelse er utilgjengelig", "实时翻译不可用", "即時翻譯無法使用"},
    {"%1 enabled Live Translate (%2 → %3).", "%1 slo på direkteoversettelse (%2 → %3).", "%1 开启了实时翻译（%2 → %3）。", "%1 開啟了即時翻譯（%2 → %3）。"},
    {"%1 is using Live Translate. Only one of you can translate at a time.", "%1 bruker direkteoversettelse. Bare én av dere kan oversette om gangen.", "%1 正在使用实时翻译。同一时间只能有一方翻译。", "%1 正在使用即時翻譯。同一時間只能有一方翻譯。"},
    {"%1 disabled Live Translate.", "%1 slo av direkteoversettelse.", "%1 关闭了实时翻译。", "%1 關閉了即時翻譯。"},
    {"Disconnected from %1", "Frakoblet %1", "已与 %1 断开连接", "已與 %1 中斷連線"},
    {"Disconnected", "Frakoblet", "已断开连接", "已中斷連線"},
    {"Disconnected; reconnects when %1 comes online", "Frakoblet; kobler til igjen når %1 kommer på", "已断开连接；%1 上线后将自动重新连接", "已中斷連線；%1 上線後將自動重新連線"},
    {"Disconnected; retrying in 10 seconds", "Frakoblet; prøver igjen om 10 sekunder", "已断开连接；10 秒后重试", "已中斷連線；10 秒後重試"},
    {"Re-connect to this host to start chatting", "Koble til denne verten på nytt for å chatte", "重新连接到此主机以开始聊天", "重新連線到此主機以開始聊天"},
    {"%1 (Web)", "%1 (web)", "%1（网页）", "%1（網頁）"},
    {"%1 (Disconnected)", "%1 (frakoblet)", "%1（已断开）", "%1（已中斷）"},
    {"Unknown host", "Ukjent vert", "未知主机", "未知主機"},
    {"Reconnecting to %1…", "Kobler til %1 på nytt…", "正在重新连接到 %1…", "正在重新連線到 %1…"},
    {"%1 is typing a message%2", "%1 skriver en melding%2", "%1 正在输入消息%2", "%1 正在輸入訊息%2"},
    {"Quote · 1 line", "Sitat · 1 linje", "引用 · 1 行", "引用 · 1 行"},
    {"Quote · %1 lines", "Sitat · %1 linjer", "引用 · %1 行", "引用 · %1 行"},
    {"Translate quote", "Oversett sitat", "翻译引用", "翻譯引用"},
    {"Translate this quote too; otherwise it is sent as it is.", "Oversett også dette sitatet; ellers sendes det som det er.", "同时翻译此引用；否则将按原样发送。", "同時翻譯此引用；否則將按原樣傳送。"},
    {"Remove this quote", "Fjern dette sitatet", "删除此引用", "移除此引用"},
    {"Original", "Original", "原文", "原文"},
    {"Sent translation", "Sendt oversettelse", "已发送的译文", "已傳送的譯文"},
    {"Translating incoming message...", "Oversetter innkommende melding...", "正在翻译收到的消息...", "正在翻譯收到的訊息..."},
    {"Show earlier messages", "Vis eldre meldinger", "显示更早的消息", "顯示較早的訊息"},
    {"──────── Earlier messages ────────", "──────── Tidligere meldinger ────────", "──────── 更早的消息 ────────", "──────── 較早的訊息 ────────"},
    {"Original: %1", "Original: %1", "原文：%1", "原文：%1"},
    {"Click to view the image", "Klikk for å se bildet", "点击查看图片", "點擊查看圖片"},
    {"Save as...", "Lagre som...", "另存为...", "另存為..."},
    {"Overwrite file?", "Overskrive filen?", "覆盖文件？", "覆寫檔案？"},
    {"The selected file already exists. Overwrite it?", "Den valgte filen finnes allerede. Vil du overskrive den?", "所选文件已存在。要覆盖吗？", "所選檔案已存在。要覆寫嗎？"},
    {"Save failed", "Lagring mislyktes", "保存失败", "儲存失敗"},
    {"Cannot save the selected file.", "Kan ikke lagre den valgte filen.", "无法保存所选文件。", "無法儲存所選檔案。"},

    // Personal PIN
    {"Enter your PIN", "Skriv inn PIN-koden", "输入您的 PIN 码", "輸入您的 PIN 碼"},
    {"Personal PIN", "Personlig PIN-kode", "个人 PIN 码", "個人 PIN 碼"},
    {"Protect %1 with a personal 4-digit PIN?\n\nWith the PIN, in addition to the central host's password, you can use %1 on several devices at once, such as LANTalk on your computer and the web client on your phone. Everyone sees one %1, and every device gets your conversations.\n\nNobody can sign in as %1 without the PIN, and the central host keeps your conversations so a new device can catch up.",
     "Vil du beskytte %1 med en personlig firesifret PIN-kode?\n\nMed PIN-koden, i tillegg til passordet til den sentrale verten, kan du bruke %1 på flere enheter samtidig, for eksempel LANTalk på datamaskinen og webklienten på mobilen. Alle ser én %1, og alle enhetene dine får samtalene.\n\nIngen kan logge inn som %1 uten PIN-koden, og den sentrale verten tar vare på samtalene dine slik at en ny enhet kan ta igjen det den gikk glipp av.",
     "要用个人 4 位数 PIN 码保护 %1 吗？\n\n有了 PIN 码，再加上中心主机的密码，您就可以同时在多台设备上使用 %1，例如电脑上的 LANTalk 和手机上的网页客户端。大家看到的是同一个 %1，每台设备都会收到您的对话。\n\n没有 PIN 码，任何人都无法以 %1 登录；中心主机会保存您的对话，新设备可以补上之前的内容。",
     "要用個人 4 位數 PIN 碼保護 %1 嗎？\n\n有了 PIN 碼，再加上中央主機的密碼，您就可以同時在多台裝置上使用 %1，例如電腦上的 LANTalk 和手機上的網頁用戶端。大家看到的是同一個 %1，每台裝置都會收到您的對話。\n\n沒有 PIN 碼，任何人都無法以 %1 登入；中央主機會保存您的對話，新裝置可以補上先前的內容。"},
    {"%1 is protected with a PIN. Enter it to use %1 on this device too.", "%1 er beskyttet med en PIN-kode. Skriv den inn for å bruke %1 på denne enheten også.", "%1 受 PIN 码保护。输入 PIN 码即可在此设备上使用 %1。", "%1 受 PIN 碼保護。輸入 PIN 碼即可在此裝置上使用 %1。"},
    {"Choose a new PIN for %1. Your other devices need it the next time they sign in.", "Velg en ny PIN-kode for %1. De andre enhetene dine trenger den neste gang de logger inn.", "为 %1 选择新的 PIN 码。您的其他设备下次登录时需要使用它。", "為 %1 選擇新的 PIN 碼。您的其他裝置下次登入時需要使用它。"},
    {"PIN:", "PIN-kode:", "PIN 码：", "PIN 碼："},
    {"New PIN:", "Ny PIN-kode:", "新 PIN 码：", "新 PIN 碼："},
    {"Repeat PIN:", "Gjenta PIN-koden:", "再次输入 PIN 码：", "再次輸入 PIN 碼："},
    {"The PINs are not the same.", "PIN-kodene er ikke like.", "两次输入的 PIN 码不一致。", "兩次輸入的 PIN 碼不一致。"},
    {"Create PIN", "Lag PIN-kode", "创建 PIN 码", "建立 PIN 碼"},
    {"Not now", "Ikke nå", "以后再说", "以後再說"},
    {"Personal PIN...", "Personlig PIN-kode...", "个人 PIN 码...", "個人 PIN 碼..."},
    {"Change PIN...", "Endre PIN-kode...", "更改 PIN 码...", "變更 PIN 碼..."},
    {"PIN changed. Use the new PIN on your other devices.", "PIN-koden er endret. Bruk den nye PIN-koden på de andre enhetene dine.", "PIN 码已更改。请在其他设备上使用新的 PIN 码。", "PIN 碼已變更。請在其他裝置上使用新的 PIN 碼。"},
    {"Your nickname is protected. Sign in with the PIN on your other devices.", "Kallenavnet ditt er beskyttet. Logg inn med PIN-koden på de andre enhetene dine.", "您的昵称已受保护。请在其他设备上使用 PIN 码登录。", "您的暱稱已受保護。請在其他裝置上使用 PIN 碼登入。"},
    {"Wrong PIN for %1. Try again.", "Feil PIN-kode for %1. Prøv igjen.", "%1 的 PIN 码错误。请重试。", "%1 的 PIN 碼錯誤。請再試一次。"},
    {"Too many wrong PINs for %1. Wait a few minutes and try again.", "For mange feil PIN-koder for %1. Vent noen minutter og prøv igjen.", "%1 的错误 PIN 码次数过多。请等待几分钟后重试。", "%1 的錯誤 PIN 碼次數過多。請等待幾分鐘後再試。"},
    {"Not connected to the central host.", "Ikke tilkoblet den sentrale verten.", "未连接到中心主机。", "未連線到中央主機。"},
    {"%1 devices", "%1 enheter", "%1 台设备", "%1 台裝置"},
    {"Live Translate was turned off on another device.", "Live Translate ble slått av på en annen enhet.", "Live Translate 已在另一台设备上关闭。", "Live Translate 已在另一台裝置上關閉。"},
    // Qt's own dialog buttons and text field context menus, which LANTalk
    // does not ship Qt's translations for.
    {"OK", "OK", "确定", "確定"},
    {"Save", "Lagre", "保存", "儲存"},
    {"Cancel", "Avbryt", "取消", "取消"},
    {"&Yes", "&Ja", "是(&Y)", "是(&Y)"},
    {"&No", "&Nei", "否(&N)", "否(&N)"},
    {"&Undo", "&Angre", "撤销(&U)", "復原(&U)"},
    {"&Redo", "&Gjør om", "重做(&R)", "重做(&R)"},
    {"Cu&t", "Klipp &ut", "剪切(&T)", "剪下(&T)"},
    {"&Copy", "&Kopier", "复制(&C)", "複製(&C)"},
    {"Copy &Link Location", "Kopier &lenkeadresse", "复制链接地址(&L)", "複製連結位址(&L)"},
    {"&Paste", "&Lim inn", "粘贴(&P)", "貼上(&P)"},
    {"Delete", "Slett", "删除", "刪除"},
    {"Select All", "Merk alt", "全选", "全選"},
};

const QHash<QString, QString> &translationTable(const QString &code) {
  static const auto tables = [] {
    QHash<QString, QHash<QString, QString>> result;
    for (const auto &row : Translations) {
      const auto source = QString::fromUtf8(row.source);
      result[Norwegian].insert(source, QString::fromUtf8(row.norwegian));
      result[SimplifiedChinese].insert(source,
                                       QString::fromUtf8(row.simplifiedChinese));
      result[TraditionalChinese].insert(
          source, QString::fromUtf8(row.traditionalChinese));
    }
    return result;
  }();
  static const QHash<QString, QString> none;
  const auto table = tables.constFind(code);
  return table == tables.cend() ? none : *table;
}

// Looks texts up by their English source alone; the context does not matter
// because each source text has one meaning throughout LANTalk.
class UiTranslator final : public QTranslator {
public:
  explicit UiTranslator(const QString &code, QObject *parent)
      : QTranslator(parent), table_(translationTable(code)) {}

  QString translate(const char *, const char *sourceText, const char *,
                    int) const override {
    return table_.value(QString::fromUtf8(sourceText));
  }

  bool isEmpty() const override { return table_.isEmpty(); }

private:
  const QHash<QString, QString> &table_;
};

bool isSupported(const QString &code) {
  for (const auto &language : uiLanguages()) {
    if (language.code == code) return true;
  }
  return false;
}

// A five-pointed star with one point aimed along the given angle (radians,
// 0 = right, counterclockwise on screen).
QPolygonF star(QPointF center, double radius, double angle) {
  QPolygonF points;
  for (int index = 0; index < 10; ++index) {
    const double r = index % 2 == 0 ? radius : radius * 0.382;
    const double a = angle + index * M_PI / 5;
    points << QPointF(center.x() + r * std::cos(a),
                      center.y() - r * std::sin(a));
  }
  return points;
}

// Flags are painted on a 30x20 canvas.
void paintUnitedKingdom(QPainter &painter) {
  painter.fillRect(QRectF(0, 0, 30, 20), QColor(0x01, 0x21, 0x69));
  painter.setClipRect(QRectF(0, 0, 30, 20));
  painter.setPen(QPen(Qt::white, 4));
  painter.drawLine(QPointF(0, 0), QPointF(30, 20));
  painter.drawLine(QPointF(30, 0), QPointF(0, 20));
  const QColor red(0xc8, 0x10, 0x2e);
  painter.setPen(QPen(red, 1.3));
  painter.drawLine(QPointF(0, 0), QPointF(30, 20));
  painter.drawLine(QPointF(30, 0), QPointF(0, 20));
  painter.setPen(Qt::NoPen);
  painter.fillRect(QRectF(12.5, 0, 5, 20), Qt::white);
  painter.fillRect(QRectF(0, 7, 30, 6), Qt::white);
  painter.fillRect(QRectF(13.5, 0, 3, 20), red);
  painter.fillRect(QRectF(0, 8.2, 30, 3.6), red);
}

void paintNorway(QPainter &painter) {
  // 22x16 units, stretched to the canvas.
  painter.scale(30.0 / 22.0, 20.0 / 16.0);
  painter.fillRect(QRectF(0, 0, 22, 16), QColor(0xba, 0x0c, 0x2f));
  painter.fillRect(QRectF(6, 0, 4, 16), Qt::white);
  painter.fillRect(QRectF(0, 6, 22, 4), Qt::white);
  const QColor blue(0x00, 0x20, 0x5b);
  painter.fillRect(QRectF(7, 0, 2, 16), blue);
  painter.fillRect(QRectF(0, 7, 22, 2), blue);
}

void paintChina(QPainter &painter) {
  painter.fillRect(QRectF(0, 0, 30, 20), QColor(0xee, 0x1c, 0x25));
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0xff, 0xff, 0x00));
  const QPointF big(5, 5);
  painter.drawPolygon(star(big, 3, M_PI / 2));
  for (const QPointF small : {QPointF(10, 2), QPointF(12, 4), QPointF(12, 7),
                              QPointF(10, 9)}) {
    const double towardBig =
        std::atan2(-(big.y() - small.y()), big.x() - small.x());
    painter.drawPolygon(star(small, 1, towardBig));
  }
}

void paintTaiwan(QPainter &painter) {
  painter.fillRect(QRectF(0, 0, 30, 20), QColor(0xfe, 0x00, 0x00));
  const QColor blue(0x00, 0x00, 0x95);
  painter.fillRect(QRectF(0, 0, 15, 10), blue);
  painter.setPen(Qt::NoPen);
  const QPointF sun(7.5, 5);
  QPolygonF rays;
  for (int index = 0; index < 24; ++index) {
    const double r = index % 2 == 0 ? 3.75 : 2.3;
    const double a = M_PI / 2 + index * M_PI / 12;
    rays << QPointF(sun.x() + r * std::cos(a), sun.y() - r * std::sin(a));
  }
  painter.setBrush(Qt::white);
  painter.drawPolygon(rays);
  painter.setBrush(blue);
  painter.drawEllipse(sun, 2.1, 2.1);
  painter.setBrush(Qt::white);
  painter.drawEllipse(sun, 1.8, 1.8);
}

} // namespace

QList<UiLanguage> uiLanguages() {
  return {
      {English, QStringLiteral("English")},
      {Norwegian, QStringLiteral("Norsk")},
      {SimplifiedChinese, QStringLiteral("简体中文")},
      {TraditionalChinese, QStringLiteral("繁體中文")},
  };
}

QString systemUiLanguage() {
  for (const auto &name : QLocale::system().uiLanguages()) {
    const QLocale locale(name);
    switch (locale.language()) {
    case QLocale::English:
      return English;
    case QLocale::NorwegianBokmal:
    case QLocale::NorwegianNynorsk:
      return Norwegian;
    case QLocale::Chinese: {
      const auto territory = locale.territory();
      return locale.script() == QLocale::TraditionalChineseScript ||
                     territory == QLocale::Taiwan ||
                     territory == QLocale::HongKong ||
                     territory == QLocale::Macao
                 ? TraditionalChinese
                 : SimplifiedChinese;
    }
    default:
      break;
    }
  }
  return English;
}

QString configuredUiLanguage() {
  const auto configured = QSettings().value(LanguageKey).toString();
  return isSupported(configured) ? configured : systemUiLanguage();
}

void setConfiguredUiLanguage(const QString &code) {
  QSettings().setValue(LanguageKey, code);
}

QIcon uiLanguageFlag(const QString &code) {
  QIcon icon;
  for (const int scale : {1, 2}) {
    QPixmap pixmap(24 * scale, 16 * scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(pixmap.width() / 30.0, pixmap.height() / 20.0);
    if (code == Norwegian) paintNorway(painter);
    else if (code == SimplifiedChinese) paintChina(painter);
    else if (code == TraditionalChinese) paintTaiwan(painter);
    else paintUnitedKingdom(painter);
    icon.addPixmap(pixmap);
  }
  return icon;
}

void installUiLanguage(QApplication &application) {
  const auto code = configuredUiLanguage();
  if (code == English) return;
  application.installTranslator(new UiTranslator(code, &application));
}

QString uiText(const QString &code, const char *source) {
  const auto text = QString::fromUtf8(source);
  return translationTable(code).value(text, text);
}
