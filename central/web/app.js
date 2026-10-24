// LANTalk web client, served by lantalkd. It signs in to the service like a
// LANTalk client (LANTALK CENTRAL protocol over a WebSocket) and speaks
// LANCHAT PROTOCOL on each relayed channel, so desktop users see web users
// as ordinary hosts. See README.md, "Central protocol" and "Protocol
// extensions".
"use strict";

// ------------------------------------------------------------- storage --

// Private browsing and blocked site data make storage throw; LANTalk then
// just forgets more.
const storage = {
  get(key, session = false) {
    try { return (session ? sessionStorage : localStorage).getItem(key); }
    catch { return null; }
  },
  // False when the value could not be stored, e.g. because storage is full.
  set(key, value, session = false) {
    try {
      const store = session ? sessionStorage : localStorage;
      if (value === null) store.removeItem(key); else store.setItem(key, value);
      return true;
    } catch {
      return false;
    }
  },
};

// ---------------------------------------------------------------- texts --

const LANGUAGES = [
  ["en", "🇬🇧", "English"],
  ["nb", "🇳🇴", "Norsk"],
  ["zh_CN", "🇨🇳", "简体中文"],
  ["zh_TW", "🇹🇼", "繁體中文"],
];

const TEXTS = {
  nickname: ["Nickname", "Kallenavn", "昵称", "暱稱"],
  password: ["Password", "Passord", "密码", "密碼"],
  passwordHint: ["Leave empty if the server has no password.",
                 "La stå tomt hvis serveren ikke har passord.",
                 "如果服务器没有密码，请留空。", "如果伺服器沒有密碼，請留空。"],
  remember: ["Remember me on this device", "Husk meg på denne enheten",
             "在此设备上记住我", "在此裝置上記住我"],
  signIn: ["Sign in", "Logg inn", "登录", "登入"],
  signingIn: ["Signing in…", "Logger inn…", "正在登录…", "正在登入…"],
  signOut: ["Sign out", "Logg ut", "退出登录", "登出"],
  search: ["Search people", "Søk etter folk", "搜索用户", "搜尋使用者"],
  back: ["Back", "Tilbake", "返回", "返回"],
  attach: ["Attach file", "Legg ved fil", "附加文件", "附加檔案"],
  message: ["Message", "Melding", "消息", "訊息"],
  send: ["Send", "Send", "发送", "傳送"],
  pickSomeone: ["Pick someone to chat with.", "Velg noen å chatte med.",
                "选择一个人开始聊天。", "選擇一個人開始聊天。"],
  nobodyOnline: ["Nobody else is online right now.",
                 "Ingen andre er pålogget akkurat nå.",
                 "目前没有其他人在线。", "目前沒有其他人在線上。"],
  noMatches: ["Nobody matches “%1”.", "Ingen treff for «%1».",
              "没有与“%1”匹配的用户。", "沒有與「%1」相符的使用者。"],
  online: ["Online", "Pålogget", "在线", "在線上"],
  away: ["Away", "Borte", "离开", "離開"],
  offline: ["Offline", "Frakoblet", "离线", "離線"],
  connecting: ["Connecting…", "Kobler til…", "正在连接…", "正在連線…"],
  reconnecting: ["Connection lost. Reconnecting…", "Mistet forbindelsen. Kobler til igjen…",
                 "连接已断开，正在重新连接…", "連線中斷，正在重新連線…"],
  typing: ["%1 is typing…", "%1 skriver…", "%1 正在输入…", "%1 正在輸入…"],
  typingShort: ["typing…", "skriver…", "正在输入…", "正在輸入…"],
  connectedTo: ["Connected to %1", "Tilkoblet %1", "已连接到 %1", "已連線到 %1"],
  disconnectedFrom: ["Disconnected from %1", "Frakoblet %1", "已与 %1 断开连接", "已與 %1 中斷連線"],
  notOnline: ["%1 is not online. Messages can be sent when they are back.",
              "%1 er ikke pålogget. Du kan sende meldinger når de er tilbake.",
              "%1 不在线。对方上线后才能发送消息。", "%1 不在線上。對方上線後才能傳送訊息。"],
  enabledTranslate: ["%1 enabled Live Translate (%2 → %3).",
                     "%1 slo på direkteoversettelse (%2 → %3).",
                     "%1 开启了实时翻译（%2 → %3）。", "%1 開啟了即時翻譯（%2 → %3）。"],
  disabledTranslate: ["%1 disabled Live Translate.", "%1 slo av direkteoversettelse.",
                      "%1 关闭了实时翻译。", "%1 關閉了即時翻譯。"],
  original: ["Original", "Original", "原文", "原文"],
  hideOriginal: ["Hide original", "Skjul original", "隐藏原文", "隱藏原文"],
  you: ["You", "Deg", "你", "你"],
  sentFile: ["Sent a file: %1", "Sendte en fil: %1", "发送了文件：%1", "傳送了檔案：%1"],
  fileTooLarge: ["Files larger than %1 cannot be sent.",
                 "Filer større enn %1 kan ikke sendes.",
                 "无法发送大于 %1 的文件。", "無法傳送大於 %1 的檔案。"],
  olderLanTalk: ["%1 uses an older LANTalk, which accepts files up to %2.",
                 "%1 bruker en eldre LANTalk, som tar imot filer på opptil %2.",
                 "%1 使用的是旧版 LANTalk，只接受不超过 %2 的文件。",
                 "%1 使用的是舊版 LANTalk，只接受不超過 %2 的檔案。"],
  noFiles: ["This server does not relay files.", "Denne serveren videresender ikke filer.",
            "此服务器不转发文件。", "此伺服器不轉送檔案。"],
  uploading: ["Uploading %1 to %2. %3%…", "Laster opp %1 til %2. %3%…",
              "正在向 %2 上传 %1。%3%…", "正在向 %2 上傳 %1。%3%…"],
  receiving: ["Receiving %1 from %2. %3%…", "Mottar %1 fra %2. %3%…",
              "正在接收来自 %2 的 %1。%3%…", "正在接收來自 %2 的 %1。%3%…"],
  sendFailed: ["Could not send %1 to %2.", "Kunne ikke sende %1 til %2.",
               "无法将 %1 发送给 %2。", "無法將 %1 傳送給 %2。"],
  receiveFailed: ["Did not receive %1 from %2.", "Mottok ikke %1 fra %2.",
                  "未能接收来自 %2 的 %1。", "未能接收來自 %2 的 %1。"],
  today: ["Today", "I dag", "今天", "今天"],
  yesterday: ["Yesterday", "I går", "昨天", "昨天"],
  wrongPassword: ["Wrong password.", "Feil passord.", "密码错误。", "密碼錯誤。"],
  needsPassword: ["This server requires a password.", "Denne serveren krever passord.",
                  "此服务器需要密码。", "此伺服器需要密碼。"],
  cannotConnect: ["Cannot reach the LANTalk server.", "Får ikke kontakt med LANTalk-serveren.",
                  "无法连接到 LANTalk 服务器。", "無法連線到 LANTalk 伺服器。"],
  liveTranslate: ["Live Translate", "Direkteoversettelse", "实时翻译", "即時翻譯"],
  serverTranslates: ["The server translates for you.", "Serveren oversetter for deg.",
                     "服务器会为你翻译。", "伺服器會為你翻譯。"],
  serverOllamaDown: ["The server's Ollama is not answering right now.",
                     "Serverens Ollama svarer ikke akkurat nå.",
                     "服务器的 Ollama 目前没有响应。", "伺服器的 Ollama 目前沒有回應。"],
  sourceServer: ["Ollama found by the server", "Ollama funnet av serveren",
                 "服务器找到的 Ollama", "伺服器找到的 Ollama"],
  sourceDirect: ["My own Ollama address", "Min egen Ollama-adresse",
                 "我自己的 Ollama 地址", "我自己的 Ollama 位址"],
  rescan: ["Rescan", "Søk på nytt", "重新扫描", "重新掃描"],
  model: ["Model", "Modell", "模型", "模型"],
  myLanguage: ["My language", "Mitt språk", "我的语言", "我的語言"],
  peerLanguage: ["Their language", "Motpartens språk", "对方的语言", "對方的語言"],
  cancel: ["Cancel", "Avbryt", "取消", "取消"],
  start: ["Start", "Start", "开始", "開始"],
  scanning: ["The server is looking for Ollama…", "Serveren søker etter Ollama…",
             "服务器正在搜索 Ollama…", "伺服器正在搜尋 Ollama…"],
  noneFound: ["The server found no Ollama.", "Serveren fant ingen Ollama.",
              "服务器未找到 Ollama。", "伺服器找不到 Ollama。"],
  someFound: ["The server found %1 Ollama server(s).", "Serveren fant %1 Ollama-server(e).",
              "服务器找到了 %1 个 Ollama 服务器。", "伺服器找到了 %1 個 Ollama 伺服器。"],
  noDiscovery: ["This server does not look for Ollama.", "Denne serveren søker ikke etter Ollama.",
                "此服务器不搜索 Ollama。", "此伺服器不搜尋 Ollama。"],
  checkingAddress: ["Checking the address…", "Sjekker adressen…", "正在检查地址…", "正在檢查位址…"],
  modelsFound: ["%1 model(s) found.", "Fant %1 modell(er).", "找到 %1 个模型。", "找到 %1 個模型。"],
  noModels: ["Ollama answers but has no models.", "Ollama svarer, men har ingen modeller.",
             "Ollama 有响应，但没有模型。", "Ollama 有回應，但沒有模型。"],
  unreachable: ["Cannot reach Ollama at this address. Ollama must allow this page: start it with OLLAMA_ORIGINS=%1",
                "Får ikke kontakt med Ollama på denne adressen. Ollama må tillate denne siden: start den med OLLAMA_ORIGINS=%1",
                "无法连接此地址上的 Ollama。Ollama 需要允许此页面：请用 OLLAMA_ORIGINS=%1 启动它",
                "無法連線此位址上的 Ollama。Ollama 需要允許此頁面：請用 OLLAMA_ORIGINS=%1 啟動它"],
  mixedContent: ["This page uses HTTPS, so the browser blocks an Ollama address that starts with http://.",
                 "Denne siden bruker HTTPS, så nettleseren blokkerer en Ollama-adresse som starter med http://.",
                 "此页面使用 HTTPS，浏览器会阻止以 http:// 开头的 Ollama 地址。",
                 "此頁面使用 HTTPS，瀏覽器會封鎖以 http:// 開頭的 Ollama 位址。"],
  chooseOllama: ["Choose an Ollama server and model.", "Velg en Ollama-server og modell.",
                 "请选择 Ollama 服务器和模型。", "請選擇 Ollama 伺服器和模型。"],
  requesting: ["Requesting Live Translate…", "Ber om direkteoversettelse…",
               "正在请求实时翻译…", "正在請求即時翻譯…"],
  translateOn: ["Live Translate: %1 ↔ %2", "Direkteoversettelse: %1 ↔ %2",
                "实时翻译：%1 ↔ %2", "即時翻譯：%1 ↔ %2"],
  translateOff: ["Live Translate is off.", "Direkteoversettelse er av.",
                 "实时翻译已关闭。", "即時翻譯已關閉。"],
  peerAlreadyTranslates: ["Live Translate is already active for the other user.",
                          "Direkteoversettelse er allerede på hos den andre.",
                          "对方已开启实时翻译。", "對方已開啟即時翻譯。"],
  peerAskedFirst: ["The other user requested Live Translate first.",
                   "Den andre ba om direkteoversettelse først.",
                   "对方先请求了实时翻译。", "對方先請求了即時翻譯。"],
  peerTranslating: ["%1 is using Live Translate. Only one of you can translate at a time.",
                    "%1 bruker direkteoversettelse. Bare én av dere kan oversette om gangen.",
                    "%1 正在使用实时翻译。同一时间只能有一方翻译。",
                    "%1 正在使用即時翻譯。同一時間只能有一方翻譯。"],
  translating: ["Translating…", "Oversetter…", "正在翻译…", "正在翻譯…"],
  translationFailed: ["Translation failed, so the message was not sent: %1",
                      "Oversettelsen mislyktes, så meldingen ble ikke sendt: %1",
                      "翻译失败，消息未发送：%1", "翻譯失敗，訊息未傳送：%1"],
  incomingFailed: ["Could not translate a message: %1", "Kunne ikke oversette en melding: %1",
                   "无法翻译消息：%1", "無法翻譯訊息：%1"],
  sentTranslation: ["Sent translation", "Sendt oversettelse", "已发送的译文", "已傳送的譯文"],
  hideTranslation: ["Hide translation", "Skjul oversettelse", "隐藏译文", "隱藏譯文"],
  pin: ["PIN", "PIN-kode", "PIN 码", "PIN 碼"],
  pinTitle: ["Personal PIN", "Personlig PIN-kode", "个人 PIN 码", "個人 PIN 碼"],
  pinOffer: ["Protect %1 with a personal 4-digit PIN? With the PIN, in addition to the server's password, you can use %1 on several devices at once, such as here and in LANTalk on your computer. Everyone sees one %1, every device gets your conversations, and the server keeps them so a new device can catch up.",
             "Vil du beskytte %1 med en personlig firesifret PIN-kode? Med PIN-koden, i tillegg til serverens passord, kan du bruke %1 på flere enheter samtidig, for eksempel her og i LANTalk på datamaskinen. Alle ser én %1, alle enhetene dine får samtalene, og serveren tar vare på dem slik at en ny enhet kan ta igjen det den gikk glipp av.",
             "要用个人 4 位数 PIN 码保护 %1 吗？有了 PIN 码，再加上服务器密码，您就可以同时在多台设备上使用 %1，例如这里和电脑上的 LANTalk。大家看到的是同一个 %1，每台设备都会收到您的对话，服务器也会保存对话，新设备可以补上之前的内容。",
             "要用個人 4 位數 PIN 碼保護 %1 嗎？有了 PIN 碼，再加上伺服器密碼，您就可以同時在多台裝置上使用 %1，例如這裡和電腦上的 LANTalk。大家看到的是同一個 %1，每台裝置都會收到您的對話，伺服器也會保存對話，新裝置可以補上先前的內容。"],
  pinChangeText: ["Choose a new PIN for %1. Your other devices need it the next time they sign in.",
                  "Velg en ny PIN-kode for %1. De andre enhetene dine trenger den neste gang de logger inn.",
                  "为 %1 选择新的 PIN 码。您的其他设备下次登录时需要使用它。",
                  "為 %1 選擇新的 PIN 碼。您的其他裝置下次登入時需要使用它。"],
  pinMissing: ["%1 is protected with a PIN. Enter it to use %1 here too.",
               "%1 er beskyttet med en PIN-kode. Skriv den inn for å bruke %1 her også.",
               "%1 受 PIN 码保护。输入 PIN 码即可在这里使用 %1。",
               "%1 受 PIN 碼保護。輸入 PIN 碼即可在這裡使用 %1。"],
  pinWrong: ["Wrong PIN. Try again.", "Feil PIN-kode. Prøv igjen.", "PIN 码错误。请重试。", "PIN 碼錯誤。請再試一次。"],
  pinLocked: ["Too many wrong PINs. Wait a few minutes and try again.",
              "For mange feil PIN-koder. Vent noen minutter og prøv igjen.",
              "错误 PIN 码次数过多。请等待几分钟后重试。", "錯誤 PIN 碼次數過多。請等待幾分鐘後再試。"],
  newPin: ["New PIN", "Ny PIN-kode", "新 PIN 码", "新 PIN 碼"],
  repeatPin: ["Repeat PIN", "Gjenta PIN-koden", "再次输入 PIN 码", "再次輸入 PIN 碼"],
  pinFourDigits: ["A PIN is 4 digits.", "En PIN-kode er fire sifre.", "PIN 码为 4 位数字。", "PIN 碼為 4 位數字。"],
  pinMismatch: ["The PINs are not the same.", "PIN-kodene er ikke like.", "两次输入的 PIN 码不一致。", "兩次輸入的 PIN 碼不一致。"],
  createPin: ["Create PIN", "Lag PIN-kode", "创建 PIN 码", "建立 PIN 碼"],
  savePin: ["Save", "Lagre", "保存", "儲存"],
  notNow: ["Not now", "Ikke nå", "以后再说", "以後再說"],
  pinSaved: ["Your nickname is protected. Sign in with the PIN on your other devices.",
             "Kallenavnet ditt er beskyttet. Logg inn med PIN-koden på de andre enhetene dine.",
             "您的昵称已受保护。请在其他设备上使用 PIN 码登录。",
             "您的暱稱已受保護。請在其他裝置上使用 PIN 碼登入。"],
  pinChanged: ["PIN changed. Use the new PIN on your other devices.",
               "PIN-koden er endret. Bruk den nye PIN-koden på de andre enhetene dine.",
               "PIN 码已更改。请在其他设备上使用新的 PIN 码。",
               "PIN 碼已變更。請在其他裝置上使用新的 PIN 碼。"],
  devices: ["%1 devices", "%1 enheter", "%1 台设备", "%1 台裝置"],
  translateOffElsewhere: ["Live Translate was turned off on another device.",
                          "Direkteoversettelse ble slått av på en annen enhet.",
                          "实时翻译已在另一台设备上关闭。", "即時翻譯已在另一台裝置上關閉。"],
  replaced: ["You signed in with this nickname somewhere else.",
             "Du logget inn med dette kallenavnet et annet sted.",
             "你已在其他地方使用此昵称登录。", "你已在其他地方使用此暱稱登入。"],
};

// The languages Live Translate offers; their English names travel in
// TRANSLATE_REQUEST, as in LANTalk (appconfig.h, languages()).
const TRANSLATION_LANGUAGES = {
  "Norwegian Bokmål": ["Norwegian Bokmål", "Norsk bokmål", "挪威语（书面）", "挪威語（書面）"],
  "Traditional Chinese": ["Traditional Chinese", "Tradisjonell kinesisk", "繁体中文", "繁體中文"],
  "Simplified Chinese": ["Simplified Chinese", "Forenklet kinesisk", "简体中文", "簡體中文"],
  "English": ["English", "Engelsk", "英语", "英語"],
};
const languageLabel = name =>
  (TRANSLATION_LANGUAGES[name] || [name])[languageIndex(language)] || name;

const languageIndex = code => Math.max(0, LANGUAGES.findIndex(l => l[0] === code));

function detectLanguage() {
  for (const tag of navigator.languages || [navigator.language || "en"]) {
    const lower = tag.toLowerCase();
    if (/^(nb|nn|no)\b/.test(lower)) return "nb";
    if (lower.startsWith("zh")) {
      return /hant|-tw|-hk|-mo/.test(lower) ? "zh_TW" : "zh_CN";
    }
    if (lower.startsWith("en")) return "en";
  }
  return "en";
}

let language = storage.get("lantalk.language") || detectLanguage();

function t(key, ...args) {
  const row = TEXTS[key];
  let text = row ? row[languageIndex(language)] : key;
  args.forEach((arg, index) => { text = text.split(`%${index + 1}`).join(arg); });
  return text;
}

function applyTexts() {
  document.documentElement.lang = language.replace("_", "-");
  for (const element of document.querySelectorAll("[data-t]")) {
    element.textContent = t(element.dataset.t);
  }
  for (const element of document.querySelectorAll("[data-t-placeholder]")) {
    element.placeholder = t(element.dataset.tPlaceholder);
  }
  for (const element of document.querySelectorAll("[data-t-title]")) {
    element.title = t(element.dataset.tTitle);
    element.setAttribute("aria-label", element.title);
  }
}

// ------------------------------------------------------------ encoding --

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function bytesToBase64Url(bytes) {
  let binary = "";
  for (let index = 0; index < bytes.length; index += 0x8000) {
    binary += String.fromCharCode.apply(null, bytes.subarray(index, index + 0x8000));
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function base64UrlToBytes(text) {
  const normal = text.replace(/-/g, "+").replace(/_/g, "/");
  const binary = atob(normal + "===".slice((normal.length + 3) % 4));
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; ++index) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

const encodeField = text => bytesToBase64Url(encoder.encode(text));
const decodeField = text => {
  try { return decoder.decode(base64UrlToBytes(text)); } catch { return ""; }
};

// HMAC-SHA256 in plain JavaScript: crypto.subtle only exists on HTTPS pages,
// and lantalkd usually serves plain HTTP on a LAN.
function sha256(bytes) {
  const k = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const length = bytes.length;
  const padded = new Uint8Array(((length + 9 + 63) >> 6) << 6);
  padded.set(bytes);
  padded[length] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, Math.floor(length / 0x20000000));
  view.setUint32(padded.length - 4, (length << 3) >>> 0);
  const w = new Uint32Array(64);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let i = 0; i < 16; ++i) w[i] = view.getUint32(offset + i * 4);
    for (let i = 16; i < 64; ++i) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; ++i) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                  ((e & f) ^ (~e & g)) + k[i] + w[i]) >>> 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                  ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  const out = new Uint8Array(32);
  const outView = new DataView(out.buffer);
  h.forEach((value, index) => outView.setUint32(index * 4, value));
  return out;
}

function hmacSha256Hex(key, message) {
  let keyBytes = encoder.encode(key);
  if (keyBytes.length > 64) keyBytes = sha256(keyBytes);
  const inner = new Uint8Array(64 + message.length);
  const outer = new Uint8Array(64 + 32);
  for (let i = 0; i < 64; ++i) {
    const byte = keyBytes[i] || 0;
    inner[i] = byte ^ 0x36;
    outer[i] = byte ^ 0x5c;
  }
  inner.set(message, 64);
  outer.set(sha256(inner), 64);
  return Array.from(sha256(outer), b => b.toString(16).padStart(2, "0")).join("");
}

// The sign-in proof (CentralProtocol::authProof).
const authProof = (password, nonce) =>
  password ? hmacSha256Hex(password, encoder.encode("LANTALK_CENTRAL " + nonce)) : "-";

const uuid = () => (crypto.randomUUID ? crypto.randomUUID() :
  "10000000-1000-4000-8000-100000000000".replace(/[018]/g, c =>
    (c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))).toString(16)));

// CentralProtocol::pinVerifier: what the server keeps for a PIN.
const pinVerifier = pin => Array.from(sha256(encoder.encode("LANTALK_PIN " + pin)),
                                      b => b.toString(16).padStart(2, "0")).join("");
const pinProof = (verifier, nonce) =>
  verifier ? hmacSha256Hex(verifier, encoder.encode("LANTALK_PIN " + nonce)) : "-";
const isPin = pin => /^[0-9]{4}$/.test(pin);

// ConversationIdentity: the same address and nickname is the same person.
const normalizeNickname = name => name.normalize("NFKC").replace(/\s+/g, " ").trim().toLowerCase();
const conversationKey = (address, nickname) => `${address}|${normalizeNickname(nickname)}`;
// A nickname with a PIN may answer from any device, so its conversation is
// not tied to one address.
const ACCOUNT_ADDRESS = "lantalkd";
const peerKey = peer =>
  conversationKey(peer.account ? ACCOUNT_ADDRESS : peer.address, peer.nickname);

// -------------------------------------------------------------- state --

// Values the desktop client uses too (appconfig.h).
const CHAT_PORT = 45454;
const MAX_FILE_SIZE = 1024 * 1024 * 1024;
// What peers accept that do not advertise CAPABILITIES LARGEFILES.
const LEGACY_MAX_FILE_SIZE = 50 * 1024 * 1024;
// How long to wait for a peer's CAPABILITIES before sending anything;
// LANChat and early LANTalk versions never send them.
const CAPABILITIES_WAIT_MS = 1500;
const CHUNK_SIZE = 16 * 1024;
const FILE_BLOCK_SIZE = 256 * 1024;
const PING_INTERVAL_MS = 25 * 1000;
// Like LANTalk (CentralProtocol::IdleTimeoutMs): a connection that has been
// silent this long is dead, even if the browser has not noticed.
const IDLE_TIMEOUT_MS = 90 * 1000;
const CONNECT_TIMEOUT_MS = 15 * 1000;
const WAKE_CHECK_MS = 5 * 1000;
const TYPING_REFRESH_MS = 15 * 1000;
const TYPING_IDLE_MS = 60 * 1000;
const TYPING_EXPIRY_MS = 35 * 1000;
// Per conversation; the browser's storage for the page is a few megabytes.
const HISTORY_LIMIT = 1000;
// Like LANTalk (AwayAfterMs): untouched this long, the user shows as away.
const AWAY_AFTER_MS = 10 * 60 * 1000;

const state = {
  socket: null,
  nickname: "",
  password: "",
  myId: "",
  myAddress: "",
  ready: false,
  signingIn: false,
  stopped: true,
  retryDelay: 1000,
  retryTimer: 0,
  pingTimer: 0,
  lastReceived: 0,
  peers: new Map(),         // id -> {id, nickname, address, port}
  conversations: new Map(), // key -> conversation
  channels: new Map(),      // channel id -> channel
  current: null,            // key of the open conversation
  ollama: { configured: false, discovery: false, scanning: false, servers: [] },
  maxFileSize: MAX_FILE_SIZE, // lantalkd may allow less (CENTRAL_FILE_LIMIT)
  away: false,
  lastActivity: Date.now(),
  translations: new Map(),  // request id -> {resolve, reject, timer}
  account: "unknown",       // CENTRAL_ACCOUNT: none, ok, or unavailable
  accountId: "",
  synced: 0,                // the account's last history entry here
  syncing: false,
  pendingVerifier: "",      // sent with CENTRAL_PIN_SET
  pinRequired: "",          // CENTRAL_PIN_REQUIRED before a rejection
  pinOffered: false,
};

const $ = id => document.getElementById(id);

// --------------------------------------------------------- connection --

function connect() {
  clearTimeout(state.retryTimer);
  if (state.socket) return;
  state.stopped = false;
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  let socket;
  try {
    socket = new WebSocket(`${scheme}//${location.host}/ws`);
  } catch {
    failSignIn(t("cannotConnect"));
    return;
  }
  state.socket = socket;
  state.lastReceived = Date.now();
  socket.onmessage = event => {
    state.lastReceived = Date.now();
    for (const line of String(event.data).split("\n")) if (line) handleCentral(line);
  };
  socket.onclose = () => connectionLost(socket);
  // An unreachable server can leave the attempt hanging for minutes.
  setTimeout(() => {
    if (state.socket === socket && !state.ready) dropConnection();
  }, CONNECT_TIMEOUT_MS);
}

// Gives up on a connection that went quiet without closing, as when a phone
// changes networks; waiting for the browser to notice can take minutes.
function dropConnection() {
  const socket = state.socket;
  if (!socket) return;
  socket.onclose = socket.onmessage = null;
  try { socket.close(); } catch { /* already closed */ }
  connectionLost(socket);
}

function connectionLost(socket) {
  if (state.socket !== socket) return;
  state.socket = null;
  clearInterval(state.pingTimer);
  failPendingTranslations();
  const wasReady = state.ready;
  state.ready = false;
  for (const channel of state.channels.values()) channelClosed(channel, false);
  state.peers.clear();
  if (state.stopped) return;
  if (state.signingIn && !wasReady) {
    failSignIn(t("cannotConnect"));
    return;
  }
  showBanner(t("reconnecting"));
  renderPeople();
  renderChatHeader();
  state.retryTimer = setTimeout(connect, state.retryDelay);
  state.retryDelay = Math.min(state.retryDelay * 2, 15000);
}

function disconnect() {
  state.stopped = true;
  clearTimeout(state.retryTimer);
  clearInterval(state.pingTimer);
  if (state.socket) {
    const socket = state.socket;
    state.socket = null;
    socket.close();
  }
  state.ready = false;
  state.channels.clear();
  state.peers.clear();
}

function sendCentral(line) {
  if (state.socket && state.socket.readyState === WebSocket.OPEN) state.socket.send(line);
}

function handleCentral(line) {
  const parts = line.split(" ");
  const command = parts[0];
  if (command === "CENTRAL_DATA") {
    const channel = state.channels.get(parts[1]);
    if (channel) handleChat(channel, line.slice(command.length + parts[1].length + 2));
    return;
  }
  switch (command) {
    case "CENTRAL_CHALLENGE":
      state.account = "unknown";
      sendCentral(`CENTRAL_HELLO 2 ${encodeField(state.nickname)} ${CHAT_PORT} ` +
                  `${authProof(state.password, parts[1])} ` +
                  `${pinProof(storage.get(pinKey()) || "", parts[1])} ${deviceId()}`);
      break;
    case "CENTRAL_PIN_REQUIRED":
      state.pinRequired = parts[1] || "missing";
      if (state.pinRequired !== "locked") storage.set(pinKey(), null);
      break;
    case "CENTRAL_REJECT": {
      const reason = decodeField(parts[1] || "");
      const pin = state.pinRequired;
      state.pinRequired = "";
      if (pin) {
        // The password was right; the PIN is what is missing.
        failSignIn(pin === "wrong" ? t("pinWrong") : pin === "locked" ? t("pinLocked")
                     : t("pinMissing", state.nickname), false, true);
        break;
      }
      failSignIn(/requires a password/.test(reason) ? t("needsPassword") :
                 /Wrong password/.test(reason) ? t("wrongPassword") : reason, true);
      break;
    }
    case "CENTRAL_ACCOUNT":
      if (parts[1] === "ok" && parts[2]) {
        state.account = "ok";
        state.accountId = parts[2];
        if (state.pendingVerifier) {
          storage.set(pinKey(), state.pendingVerifier);
          state.pendingVerifier = "";
          showNotice(t("pinSaved"));
        }
        state.synced = Number(storage.get(`lantalk.synced.${state.accountId}`)) || 0;
        // Before any conversation opens, so the history stays in order.
        state.syncing = true;
        sendCentral(`CENTRAL_SYNC ${state.synced}`);
      } else if (parts[1] === "none" || parts[1] === "unavailable") {
        state.account = parts[1];
        state.pendingVerifier = "";
      }
      renderPinButton();
      if (state.ready) offerPin();
      break;
    case "CENTRAL_PIN_CHANGED":
      if (state.pendingVerifier) storage.set(pinKey(), state.pendingVerifier);
      state.pendingVerifier = "";
      showNotice(t("pinChanged"));
      break;
    case "CENTRAL_HISTORY":
      addHistoryEntry(parts[1] || "");
      break;
    case "CENTRAL_SEQ":
      noteSequence(Number(parts[1]));
      break;
    case "CENTRAL_SYNC_DONE":
      noteSequence(Number(parts[1]));
      state.syncing = false;
      saveHistory();
      if (state.ready) { renderPeople(); renderChat(); }
      break;
    case "CENTRAL_MIRROR": {
      const channel = state.channels.get(parts[1]);
      if (channel) handleMirror(channel, line.slice(command.length + parts[1].length + 2));
      break;
    }
    case "CENTRAL_PEER_ACCOUNT": {
      const peer = state.peers.get(parts[1]);
      if (peer) {
        const before = peerKey(peer);
        peer.account = true;
        peer.devices = Math.max(1, Number(parts[2]) || 1);
        if (before !== peerKey(peer)) adoptConversation(before, peerKey(peer));
        peerArrived(peer);
      }
      break;
    }
    case "CENTRAL_REPLACED":
      failSignIn(t("replaced"));
      break;
    case "CENTRAL_PEER":
      if (parts.length === 5) {
        const peer = { id: parts[1], nickname: decodeField(parts[2]),
                       address: parts[3], port: Number(parts[4]) };
        if (peer.nickname && peer.id !== state.myId) {
          const known = state.peers.get(peer.id);
          if (known) Object.assign(peer, { account: known.account, devices: known.devices });
          state.peers.set(peer.id, peer);
          peerArrived(peer);
        }
      }
      break;
    case "CENTRAL_PEER_STATUS": {
      const peer = state.peers.get(parts[1]);
      if (peer) {
        peer.away = parts[2] === "away";
        if (state.ready) { renderPeople(); renderChatHeader(); }
      }
      break;
    }
    case "CENTRAL_PEER_KIND": {
      const peer = state.peers.get(parts[1]);
      if (peer) {
        peer.web = parts[2] === "web";
        if (state.ready) { renderPeople(); renderChatHeader(); }
      }
      break;
    }
    case "CENTRAL_PEER_GONE":
      if (state.peers.delete(parts[1]) && state.ready) {
        renderPeople();
        renderChatHeader();
      }
      break;
    case "CENTRAL_WELCOME":
      state.myId = parts[1];
      state.myAddress = parts[2] || "";
      state.ready = true;
      state.retryDelay = 1000;
      // The service takes everyone for active until told otherwise.
      if (state.away) sendCentral("CENTRAL_STATUS away");
      clearInterval(state.pingTimer);
      state.pingTimer = setInterval(() => {
        if (Date.now() - state.lastReceived > IDLE_TIMEOUT_MS) dropConnection();
        else sendCentral("CENTRAL_PING");
      }, PING_INTERVAL_MS);
      signedIn();
      break;
    case "CENTRAL_INCOMING": {
      const peer = state.peers.get(parts[2]);
      if (!peer || state.channels.has(parts[1])) {
        sendCentral(`CENTRAL_CLOSE ${parts[1]} unavailable`);
        break;
      }
      const conversation = conversationFor(peer, true);
      const channel = addChannel(parts[1], conversation, true);
      sendChat(channel, `NICKNAME ${protocolNickname()}`);
      break;
    }
    case "CENTRAL_OPENED": {
      const channel = state.channels.get(parts[1]);
      if (channel) {
        channel.open = true;
        sendChat(channel, `NICKNAME ${protocolNickname()}`);
      }
      break;
    }
    case "CENTRAL_CLOSE": {
      const channel = state.channels.get(parts[1]);
      if (channel) channelClosed(channel, true);
      break;
    }
    case "CENTRAL_FILE_LIMIT": {
      const limit = Number(parts[1]);
      state.maxFileSize = Number.isSafeInteger(limit) && limit >= 0
        ? Math.min(limit, MAX_FILE_SIZE) : MAX_FILE_SIZE;
      $("composer").querySelector(".attach").hidden = state.maxFileSize === 0;
      break;
    }
    case "CENTRAL_FILE_REJECTED": {
      const channel = state.channels.get(parts[1]);
      const message = channel && channel.outgoingFiles.get(parts[2]);
      if (message) {
        channel.outgoingFiles.delete(parts[2]);
        message.transfer = message.transfer ||
          { name: message.file.name, peer: channel.key && state.conversations.get(channel.key).nickname };
        failTransfer(message);
        const conversation = state.conversations.get(channel.key);
        const limit = Number(parts[3]);
        if (conversation) {
          addNotice(conversation, limit ? t("fileTooLarge", formatSize(limit)) : t("noFiles"));
        }
      }
      break;
    }
    case "CENTRAL_OLLAMA":
      try {
        state.ollama = Object.assign({ servers: [] }, JSON.parse(decodeField(parts[1] || "")));
      } catch { /* keep the previous state */ }
      renderTranslateButton();
      if (!$("translate-sheet").hidden) renderTranslateSheet(false);
      break;
    case "CENTRAL_TRANSLATED":
    case "CENTRAL_TRANSLATE_FAILED": {
      const pending = state.translations.get(parts[1]);
      if (!pending) break;
      state.translations.delete(parts[1]);
      clearTimeout(pending.timer);
      if (command === "CENTRAL_TRANSLATED") pending.resolve(decodeField(parts[2] || ""));
      else pending.reject(new Error(decodeField(parts[2] || "")));
      break;
    }
    default:
      break; // CENTRAL_PONG and newer commands.
  }
}

// Messages written while they were away go out now, and an open
// conversation picks up again.
function peerArrived(peer) {
  const conversation = conversationFor(peer, false);
  if (conversation && (conversation.queue.length || state.current === conversation.key)) {
    ensureChannel(conversation);
  }
  if (state.ready) { renderPeople(); renderChatHeader(); }
}

// The conversation so far was with one address; it continues under the key
// of the nickname's account.
function adoptConversation(from, to) {
  const old = state.conversations.get(from);
  if (!old) return;
  state.conversations.delete(from);
  const existing = state.conversations.get(to);
  if (existing) {
    existing.messages = old.messages.concat(existing.messages);
    existing.queue = old.queue.concat(existing.queue);
  } else {
    old.key = to;
    old.address = ACCOUNT_ADDRESS;
    state.conversations.set(to, old);
  }
  for (const channel of state.channels.values()) if (channel.key === from) channel.key = to;
  if (state.current === from) state.current = to;
  saveHistory();
}

// ------------------------------------------------------- personal PIN --

const pinKey = () => `lantalk.pin.${normalizeNickname(state.nickname)}`;
const pinDeclinedKey = () => `lantalk.pinDeclined.${normalizeNickname(state.nickname)}`;

function deviceId() {
  let id = storage.get("lantalk.device");
  if (!id || !/^[A-Za-z0-9-]{1,64}$/.test(id)) {
    id = uuid();
    storage.set("lantalk.device", id);
  }
  return id;
}

function noteSequence(sequence) {
  if (!state.accountId || !Number.isSafeInteger(sequence) || sequence <= state.synced) return;
  state.synced = sequence;
  storage.set(`lantalk.synced.${state.accountId}`, String(sequence));
}

// A message of the account's another device had, or one sent while this
// device was away.
function addHistoryEntry(encoded) {
  let entry;
  try { entry = JSON.parse(decodeField(encoded)); } catch { return; }
  if (!entry || !entry.peer || !entry.message || !(entry.seq > state.synced)) return;
  noteSequence(entry.seq);
  const address = entry.account ? ACCOUNT_ADDRESS : String(entry.address || "");
  const key = conversationKey(address, entry.peer);
  const conversation = state.conversations.get(key) || newConversation(key, entry.peer, address);
  const time = Date.parse(entry.time) || Date.now();
  const message = entry.out
    ? { out: true, text: entry.message, translation: entry.alternate || undefined, time }
    : { out: false, text: entry.message, original: entry.alternate || undefined, time };
  if (state.syncing) {
    // In order among what this device already has.
    let index = conversation.messages.length;
    while (index > 0 && (conversation.messages[index - 1].time || 0) > time) --index;
    conversation.messages.splice(index, 0, message);
    conversation.lastActivity = Math.max(conversation.lastActivity, time);
    return;
  }
  addMessage(conversation, message);
}

function renderPinButton() {
  $("pin-button").hidden = !(state.account === "none" || state.account === "ok");
}

// Once per sign-in, until "Not now".
function offerPin() {
  if (state.account !== "none" || state.pinOffered || storage.get(pinDeclinedKey())) return;
  state.pinOffered = true;
  openPinSheet();
}

function openPinSheet() {
  const changing = state.account === "ok";
  $("pin-text").textContent = changing ? t("pinChangeText", state.nickname)
                                       : t("pinOffer", state.nickname);
  $("pin-save").textContent = changing ? t("savePin") : t("createPin");
  $("pin-later").textContent = changing ? t("cancel") : t("notNow");
  $("pin-new").value = "";
  $("pin-repeat").value = "";
  $("pin-error").hidden = true;
  $("pin-sheet").hidden = false;
  $("pin-new").focus();
}

function closePinSheet(declined) {
  $("pin-sheet").hidden = true;
  if (declined && state.account === "none") storage.set(pinDeclinedKey(), "1");
}

function submitPin() {
  const pin = $("pin-new").value;
  const error = !isPin(pin) ? t("pinFourDigits")
    : pin !== $("pin-repeat").value ? t("pinMismatch") : "";
  $("pin-error").textContent = error;
  $("pin-error").hidden = !error;
  if (error) return;
  state.pendingVerifier = pinVerifier(pin);
  sendCentral(`CENTRAL_PIN_SET ${state.pendingVerifier}`);
  closePinSheet(false);
}

let noticeTimer = 0;
function showNotice(text) {
  showBanner(text);
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => { if (state.ready) showBanner(""); }, 6000);
}

// escapeProtocolField: one line, single spaces.
const protocolNickname = () => state.nickname.replace(/\s+/g, " ").trim();

// ------------------------------------------------------- conversations --

function conversationFor(peer, create) {
  const key = peerKey(peer);
  let conversation = state.conversations.get(key);
  if (!conversation && create !== false) {
    conversation = newConversation(key, peer.nickname,
                                   peer.account ? ACCOUNT_ADDRESS : peer.address);
  }
  return conversation;
}

function newConversation(key, nickname, address) {
  const conversation = {
    key, nickname, address,
    messages: [],
    unread: 0,
    channel: null,
    queue: [],
    peerTyping: false,
    peerTypingTimer: 0,
    localTyping: false,
    localTypingRefresh: 0,
    localTypingIdle: 0,
    lastActivity: 0,
    translating: false,       // we translate, granted by the peer
    translateRequest: null,   // {id, automatic} while asking
    peerTranslating: false,   // the peer translates; we may not
  };
  state.conversations.set(key, conversation);
  return conversation;
}

// The online peer for a conversation, newest registration first.
function peerFor(conversation) {
  let found = null;
  for (const peer of state.peers.values()) {
    if (peerKey(peer) === conversation.key) found = peer;
  }
  return found;
}

function addChannel(id, conversation, open) {
  const channel = { id, key: conversation.key, open, handshake: false,
                    supportsTyping: false, files: new Map(), outgoingFiles: new Map() };
  state.channels.set(id, channel);
  // A newer channel to the same person takes over sending.
  conversation.channel = id;
  return channel;
}

function currentChannel(conversation) {
  const channel = conversation.channel && state.channels.get(conversation.channel);
  return channel || null;
}

function ensureChannel(conversation) {
  if (currentChannel(conversation) || !state.ready) return;
  const peer = peerFor(conversation);
  if (!peer) return;
  const channel = addChannel(uuid(), conversation, false);
  sendCentral(`CENTRAL_OPEN ${channel.id} ${peer.id}`);
}

function failTransfer(message) {
  if (!message || !message.transfer) return;
  message.transfer.failed = true;
  message.pending = false;
  refreshMessageElement(message);
}

function channelClosed(channel, notify) {
  state.channels.delete(channel.id);
  for (const file of channel.files.values()) failTransfer(file.message);
  for (const message of channel.outgoingFiles.values()) failTransfer(message);
  const conversation = state.conversations.get(channel.key);
  if (!conversation || conversation.channel !== channel.id) return;
  conversation.channel = null;
  conversation.translating = false;
  conversation.translateRequest = null;
  conversation.peerTranslating = false;
  conversation.peerAway = false;
  setPeerTyping(conversation, false);
  stopLocalTyping(conversation);
  if (channel.handshake && notify) {
    addNotice(conversation, t("disconnectedFrom", conversation.nickname));
  }
  if (state.current === conversation.key) renderChatHeader();
  renderTranslateButton();
}

function sendChat(channel, line) {
  sendCentral(`CENTRAL_DATA ${channel.id} ${line}`);
}

// Sends a LANCHAT line to the person, once the channel's handshake is done.
// `line` may be a function of the channel, returning the line or null to
// send nothing, for lines that depend on what the peer supports.
function sendToConversation(conversation, line, onSent) {
  const channel = currentChannel(conversation);
  if (channel && channel.handshake && channel.sendReady) {
    sendItem(channel, { line, onSent });
    return true;
  }
  conversation.queue.push({ line, onSent });
  ensureChannel(conversation);
  const notice = t("notOnline", conversation.nickname);
  const last = conversation.messages[conversation.messages.length - 2];
  if (!peerFor(conversation) && !(last && last.notice === notice)) {
    addNotice(conversation, notice);
  }
  return false;
}

function handleChat(channel, line) {
  const conversation = state.conversations.get(channel.key);
  if (!conversation) return;
  if (line.startsWith("NICKNAME ")) {
    const name = line.slice(9).trim();
    if (!name) return;
    const first = !channel.handshake;
    channel.handshake = true;
    if (first) {
      sendChat(channel, "CAPABILITIES TYPING LARGEFILES PRESENCE");
      channel.capabilitiesTimer =
        setTimeout(() => readyToSend(conversation, channel), CAPABILITIES_WAIT_MS);
      if (conversation.channel === channel.id) {
        addNotice(conversation, t("connectedTo", name));
        // Live Translate comes back on by itself, like in LANTalk.
        if (translationSettings(conversation).enabled) requestTranslation(conversation, true);
      }
    }
    if (state.current === conversation.key) renderChatHeader();
    renderTranslateButton();
    return;
  }
  if (!channel.handshake) return;
  const parts = line.split(" ");
  switch (parts[0]) {
    case "MESSAGE": {
      if (parts.length !== 2 && !(parts.length === 4 && parts[2] === "ORG_LANG")) return;
      const text = decodeField(parts[1]);
      const original = parts.length === 4 ? decodeField(parts[3]) : "";
      if (!text) return;
      setPeerTyping(conversation, false);
      if (conversation.translating && !original) {
        receiveTranslated(conversation, text);
        break;
      }
      addMessage(conversation, { out: false, text, original });
      announce(conversation, preview(text));
      break;
    }
    case "CAPABILITIES":
      channel.supportsTyping = parts.includes("TYPING");
      channel.largeFiles = parts.includes("LARGEFILES");
      if (!channel.presence && parts.includes("PRESENCE")) {
        channel.presence = true;
        if (state.away) sendChat(channel, "PRESENCE AWAY");
      }
      if (channel.supportsTyping && conversation.localTyping) sendChat(channel, "TYPING START");
      readyToSend(conversation, channel);
      break;
    case "PRESENCE":
      if (parts[1] === "AWAY" || parts[1] === "ACTIVE") {
        conversation.peerAway = parts[1] === "AWAY";
        renderPeople();
        renderChatHeader();
      }
      break;
    case "TYPING":
      if (parts[1] === "START") {
        channel.supportsTyping = true;
        setPeerTyping(conversation, true);
      } else if (parts[1] === "STOP") {
        setPeerTyping(conversation, false);
      }
      break;
    case "TRANSLATE_REQUEST": {
      if (parts.length !== 4) break;
      const id = parts[1];
      const source = decodeField(parts[2]);
      const target = decodeField(parts[3]);
      // ChatSession's rules: one side translates, and of two simultaneous
      // requests the smaller id wins.
      const ours = conversation.translateRequest;
      if (!source || !target || conversation.translating || conversation.peerTranslating ||
          (ours && ours.id < id)) {
        sendChat(channel, `TRANSLATE_REJECT ${id}`);
        break;
      }
      if (ours) {
        conversation.translateRequest = null;
        if (!ours.automatic) addNotice(conversation, t("peerAskedFirst"));
      }
      conversation.peerTranslating = true;
      sendChat(channel, `TRANSLATE_GRANTED ${id}`);
      addNotice(conversation, t("enabledTranslate", conversation.nickname,
                                languageLabel(target), languageLabel(source)));
      renderTranslateButton();
      break;
    }
    case "TRANSLATE_GRANTED":
      if (conversation.translateRequest && parts[1] === conversation.translateRequest.id) {
        const request = conversation.translateRequest;
        conversation.translateRequest = null;
        conversation.translating = true;
        if (request.mirrored) {
          // Turned on from another device: its languages, remembered here too.
          saveTranslationSettings(conversation, Object.assign(translationSettings(conversation),
            { enabled: true, mine: request.mine, peer: request.peer }));
        }
        const settings = translationSettings(conversation);
        addNotice(conversation, t("translateOn", languageLabel(settings.mine),
                                  languageLabel(settings.peer)));
        renderTranslateButton();
      }
      break;
    case "TRANSLATE_REJECT":
      if (conversation.translateRequest && parts[1] === conversation.translateRequest.id) {
        const automatic = conversation.translateRequest.automatic ||
                          conversation.translateRequest.mirrored;
        conversation.translateRequest = null;
        if (!automatic) addNotice(conversation, t("peerAlreadyTranslates"));
        renderTranslateButton();
      }
      break;
    case "TRANSLATE_STOP":
      if (parts.length !== 1) break;
      conversation.peerTranslating = false;
      addNotice(conversation, t("disabledTranslate", conversation.nickname));
      renderTranslateButton();
      break;
    case "TRANSLATE_CANCEL":
      if (!conversation.peerTranslating) break;
      conversation.peerTranslating = false;
      addNotice(conversation, t("disabledTranslate", conversation.nickname));
      renderTranslateButton();
      break;
    case "FILE_BEGIN":
    case "FILE_CHUNK":
    case "FILE_END":
      handleFile(channel, conversation, parts, false);
      break;
    default:
      break;
  }
}

// FILE_BEGIN, FILE_CHUNK, and FILE_END; `out` for a file another device of
// this account sent.
function handleFile(channel, conversation, parts, out) {
  if (parts[0] === "FILE_BEGIN") {
    const size = Number(parts[4]);
    const name = decodeField(parts[2] || "").split(/[\\/]/).pop();
    const mime = decodeField(parts[3] || "");
    if (parts.length !== 5 || !Number.isSafeInteger(size) || size < 0 ||
        size > MAX_FILE_SIZE || !name || name === "." || name === ".." || !mime) return;
    const message = addMessage(conversation, {
      out, transfer: { name, peer: conversation.nickname, percent: 0 } });
    channel.files.set(parts[1], { name, mime, size, received: 0, chunks: [], message });
  } else if (parts[0] === "FILE_CHUNK") {
    const file = channel.files.get(parts[1]);
    if (!file || parts.length !== 3) return;
    let chunk;
    try { chunk = base64UrlToBytes(parts[2]); } catch { chunk = new Uint8Array(); }
    if (!chunk.length || file.received + chunk.length > file.size) {
      channel.files.delete(parts[1]);
      failTransfer(file.message);
      return;
    }
    file.chunks.push(chunk);
    file.received += chunk.length;
    setTransferPercent(file.message, Math.floor(file.received * 100 / file.size));
  } else if (parts[0] === "FILE_END") {
    const file = channel.files.get(parts[1]);
    channel.files.delete(parts[1]);
    if (!file) return;
    if (file.received !== file.size) {
      failTransfer(file.message);
      return;
    }
    // The progress bubble becomes the attachment.
    const blob = new Blob(file.chunks, { type: file.mime });
    file.message.file = { name: file.name, mime: file.mime, url: URL.createObjectURL(blob) };
    delete file.message.transfer;
    refreshMessageElement(file.message);
    saveHistory();
    renderPeople();
    if (!out) announce(conversation, t("sentFile", file.name));
  }
}

// What another device of this account sent in the same conversation.
function handleMirror(channel, line) {
  const conversation = state.conversations.get(channel.key);
  if (!conversation) return;
  const parts = line.split(" ");
  switch (parts[0]) {
    case "MESSAGE": {
      if (parts.length !== 2 && !(parts.length === 4 && parts[2] === "ORG_LANG")) return;
      const text = decodeField(parts[1]);
      const original = parts.length === 4 ? decodeField(parts[3]) : "";
      if (!text) return;
      addMessage(conversation, original ? { out: true, text: original, translation: text }
                                        : { out: true, text });
      break;
    }
    case "TRANSLATE_REQUEST":
      // The peer's TRANSLATE_GRANTED comes to every device of this account.
      if (parts.length !== 4 || conversation.translating) return;
      conversation.translateRequest = { id: parts[1], mirrored: true,
                                        mine: decodeField(parts[2]), peer: decodeField(parts[3]) };
      renderTranslateButton();
      break;
    case "TRANSLATE_STOP":
    case "TRANSLATE_CANCEL":
      if (!conversation.translating && !conversation.translateRequest) return;
      conversation.translating = false;
      conversation.translateRequest = null;
      saveTranslationSettings(conversation,
                              Object.assign(translationSettings(conversation), { enabled: false }));
      addNotice(conversation, t("translateOffElsewhere"));
      renderTranslateButton();
      break;
    case "FILE_BEGIN":
    case "FILE_CHUNK":
    case "FILE_END":
      handleFile(channel, conversation, parts, true);
      break;
    default:
      break;
  }
}

function sendItem(channel, item) {
  const line = typeof item.line === "function" ? item.line(channel) : item.line;
  if (line === null) return;
  sendChat(channel, line);
  if (item.onSent) item.onSent();
}

// Once the peer's capabilities are known (or clearly will not come).
function readyToSend(conversation, channel) {
  clearTimeout(channel.capabilitiesTimer);
  if (channel.sendReady || !state.channels.has(channel.id)) return;
  channel.sendReady = true;
  if (conversation.channel !== channel.id) return;
  for (const item of conversation.queue.splice(0)) sendItem(channel, item);
}

// ------------------------------------------------------------- typing --

function setPeerTyping(conversation, typing) {
  clearTimeout(conversation.peerTypingTimer);
  if (typing) {
    conversation.peerTypingTimer =
      setTimeout(() => setPeerTyping(conversation, false), TYPING_EXPIRY_MS);
  }
  if (conversation.peerTyping === typing) return;
  conversation.peerTyping = typing;
  renderTyping();
  renderPeople();
}

function noteLocalTyping(conversation, hasDraft) {
  if (!hasDraft) {
    stopLocalTyping(conversation);
    return;
  }
  clearTimeout(conversation.localTypingIdle);
  conversation.localTypingIdle =
    setTimeout(() => stopLocalTyping(conversation), TYPING_IDLE_MS);
  if (conversation.localTyping) return;
  conversation.localTyping = true;
  sendTyping(conversation, "START");
  conversation.localTypingRefresh =
    setInterval(() => sendTyping(conversation, "START"), TYPING_REFRESH_MS);
}

function stopLocalTyping(conversation) {
  clearTimeout(conversation.localTypingIdle);
  clearInterval(conversation.localTypingRefresh);
  if (!conversation.localTyping) return;
  conversation.localTyping = false;
  sendTyping(conversation, "STOP");
}

function sendTyping(conversation, what) {
  const channel = currentChannel(conversation);
  if (channel && channel.handshake && channel.supportsTyping) {
    sendChat(channel, `TYPING ${what}`);
  }
}

// ----------------------------------------------------------- messages --

function addMessage(conversation, message) {
  message.time = message.time || Date.now();
  conversation.messages.push(message);
  if (conversation.messages.length > HISTORY_LIMIT * 2) {
    conversation.messages.splice(0, conversation.messages.length - HISTORY_LIMIT);
  }
  conversation.lastActivity = message.time;
  if (!message.out && !isVisible(conversation)) conversation.unread++;
  saveHistory();
  if (state.current === conversation.key) appendMessageElement(message, true);
  renderPeople();
  updateTitle();
  return message;
}

function addNotice(conversation, text) {
  return addMessage(conversation, { notice: text, out: true });
}

function preview(text) {
  return parseQuotes(text).map(segment =>
    segment.quote ? "❝…❞" : segment.text.replace(/\s+/g, " ")).join(" ").trim();
}

// QuotedMessage::parse: "[quote]" (or "[quote translate]") lines open a
// quote and "[/quote]" closes it.
function parseQuotes(message) {
  const segments = [];
  const open = /\[quote( translate)?\]/g;
  const trimStart = s => (s.startsWith("\n") ? s.slice(1) : s);
  const trimEnd = s => (s.endsWith("\n") ? s.slice(0, -1) : s);
  let position = 0;
  while (position < message.length) {
    open.lastIndex = position;
    const match = open.exec(message);
    const close = match ? message.indexOf("[/quote]", open.lastIndex) : -1;
    if (close < 0) {
      const rest = message.slice(position);
      segments.push({ text: segments.length ? trimStart(rest) : rest });
      break;
    }
    const before = message.slice(position, match.index);
    const text = trimEnd(segments.length ? trimStart(before) : before);
    if (text) segments.push({ text });
    segments.push({ text: trimEnd(trimStart(message.slice(open.lastIndex, close))), quote: true });
    position = close + "[/quote]".length;
  }
  if (segments.length && !segments[segments.length - 1].quote &&
      !segments[segments.length - 1].text) segments.pop();
  return segments;
}

function sendText(conversation, text) {
  if (conversation.translating) {
    sendTranslated(conversation, text);
    return;
  }
  const message = addMessage(conversation, { out: true, text, pending: true });
  const delivered = sendToConversation(conversation, `MESSAGE ${encodeField(text)}`, () => {
    message.pending = false;
    saveHistory();
    refreshMessageElement(message);
  });
  if (delivered) refreshMessageElement(message);
}

async function sendFile(conversation, file) {
  if (file.size > state.maxFileSize) {
    addNotice(conversation, state.maxFileSize
      ? t("fileTooLarge", formatSize(state.maxFileSize)) : t("noFiles"));
    return;
  }
  // The bubble shows the upload until the file is on its way.
  const message = addMessage(conversation, {
    out: true, pending: true,
    transfer: { name: file.name, peer: conversation.nickname, percent: 0 },
    file: { name: file.name, mime: file.type || "application/octet-stream",
            url: URL.createObjectURL(file) },
  });
  const id = uuid();
  const begin = channel => {
    // An older LANTalk would turn the file down without saying so.
    if (!channel.largeFiles && file.size > LEGACY_MAX_FILE_SIZE) {
      failTransfer(message);
      addNotice(conversation, t("olderLanTalk", conversation.nickname,
                                formatSize(LEGACY_MAX_FILE_SIZE)));
      return null;
    }
    return `FILE_BEGIN ${id} ${encodeField(file.name)} ` +
           `${encodeField(message.file.mime)} ${file.size}`;
  };
  sendToConversation(conversation, begin, () => pumpFile(conversation, message, id, file, 0));
}

// Reads the file a block at a time, so a large one never sits in memory,
// and sends while the connection keeps up.
async function pumpFile(conversation, message, id, file, offset) {
  const usable = () => {
    const channel = currentChannel(conversation);
    if (channel && state.socket && !message.transfer.failed) return channel;
    // The connection went away mid-file, or the server refused it.
    message.transfer.failed = true;
    message.pending = false;
    refreshMessageElement(message);
    return null;
  };
  let channel = usable();
  if (!channel) return;
  channel.outgoingFiles.set(id, message);
  if (offset < file.size && state.socket.bufferedAmount < 256 * 1024) {
    let block;
    try {
      block = new Uint8Array(await file.slice(offset, offset + FILE_BLOCK_SIZE).arrayBuffer());
    } catch {
      failTransfer(message);
      return;
    }
    channel = usable();
    if (!channel) return;
    for (let index = 0; index < block.length; index += CHUNK_SIZE) {
      sendChat(channel, `FILE_CHUNK ${id} ${bytesToBase64Url(block.subarray(index, index + CHUNK_SIZE))}`);
    }
    offset += block.length;
  }
  setTransferPercent(message, file.size ? Math.floor(offset * 100 / file.size) : 100);
  if (offset < file.size) {
    setTimeout(() => pumpFile(conversation, message, id, file, offset),
               state.socket && state.socket.bufferedAmount < 256 * 1024 ? 0 : 30);
    return;
  }
  // Kept in outgoingFiles: a small file is all sent before lantalkd can
  // refuse it.
  sendChat(channel, `FILE_END ${id}`);
  message.pending = false;
  delete message.transfer;
  refreshMessageElement(message);
}

const formatSize = bytes => bytes >= 1024 * 1024
  ? `${Math.round(bytes / 1024 / 1024 * 10) / 10} MB` : `${Math.round(bytes / 1024)} KB`;

function setTransferPercent(message, percent) {
  if (message.transfer.percent === percent) return;
  message.transfer.percent = percent;
  refreshMessageElement(message);
}

// ----------------------------------------------------- Live Translate --

// Choices per conversation, kept on this device like LANTalk keeps them per
// tab: languages, where to translate, and whether it is on.
const settingsKey = conversation =>
  `lantalk.translate.${normalizeNickname(state.nickname)}.${conversation.key}`;

function defaultLanguages() {
  const mine = { nb: "Norwegian Bokmål", zh_CN: "Simplified Chinese",
                 zh_TW: "Traditional Chinese" }[language] || "English";
  return { mine, peer: mine === "English" ? "Norwegian Bokmål" : "English" };
}

// A conversation without choices of its own starts from the last ones made.
function translationSettings(conversation) {
  const read = key => {
    try { return JSON.parse(storage.get(key) || "null"); } catch { return null; }
  };
  const own = read(settingsKey(conversation));
  return Object.assign({ source: "server", url: "", model: "" }, defaultLanguages(),
                       own || read("lantalk.translate.last") || {},
                       { enabled: Boolean(own && own.enabled) });
}

function saveTranslationSettings(conversation, settings) {
  const value = JSON.stringify(settings);
  storage.set(settingsKey(conversation), value);
  storage.set("lantalk.translate.last", JSON.stringify(Object.assign({}, settings, { enabled: false })));
}

function requestTranslation(conversation, automatic) {
  const channel = currentChannel(conversation);
  if (!channel || !channel.handshake || conversation.translating ||
      conversation.translateRequest || conversation.peerTranslating) return;
  const settings = translationSettings(conversation);
  conversation.translateRequest = { id: uuid(), automatic };
  sendChat(channel, `TRANSLATE_REQUEST ${conversation.translateRequest.id} ` +
                    `${encodeField(settings.mine)} ${encodeField(settings.peer)}`);
  if (!automatic) addNotice(conversation, t("requesting"));
  renderTranslateButton();
}

function stopTranslation(conversation) {
  const channel = currentChannel(conversation);
  if (channel && channel.handshake) {
    if (conversation.translateRequest) {
      sendChat(channel, `TRANSLATE_CANCEL ${conversation.translateRequest.id}`);
    } else if (conversation.translating) {
      sendChat(channel, "TRANSLATE_STOP");
    }
  }
  conversation.translating = false;
  conversation.translateRequest = null;
  const settings = translationSettings(conversation);
  saveTranslationSettings(conversation, Object.assign(settings, { enabled: false }));
  addNotice(conversation, t("translateOff"));
  renderTranslateButton();
}

function renderTranslateButton() {
  const button = $("translate");
  const conversation = state.conversations.get(state.current);
  if (!conversation) {
    button.hidden = true;
    return;
  }
  const channel = currentChannel(conversation);
  const active = conversation.translating || Boolean(conversation.translateRequest);
  button.hidden = false;
  button.setAttribute("aria-pressed", String(active));
  // The interlock: only one side translates.
  button.disabled = conversation.peerTranslating || !channel || !channel.handshake;
  button.title = conversation.peerTranslating
    ? t("peerTranslating", conversation.nickname) : t("liveTranslate");
  button.setAttribute("aria-label", button.title);
}

// Translates a message: through lantalkd when it has Ollama configured or
// the user picked one it found, otherwise straight from the browser.
function translateMessage(conversation, text, from, to) {
  const settings = translationSettings(conversation);
  if (state.ollama.configured) return translateOnServer(text, from, to);
  if (settings.source === "server") {
    return translateOnServer(text, from, to, settings.url, settings.model);
  }
  return translateDirectly(text, from, to, settings.url, settings.model);
}

function translateOnServer(text, source, target, url, model) {
  return new Promise((resolve, reject) => {
    const id = uuid();
    const request = { text, source, target };
    if (url) Object.assign(request, { url, model });
    const timer = setTimeout(() => {
      state.translations.delete(id);
      reject(new Error("Ollama did not answer in time."));
    }, 180000);
    state.translations.set(id, { resolve, reject, timer });
    sendCentral(`CENTRAL_TRANSLATE ${id} ${encodeField(JSON.stringify(request))}`);
  });
}

function failPendingTranslations() {
  for (const pending of state.translations.values()) {
    clearTimeout(pending.timer);
    pending.reject(new Error(t("reconnecting")));
  }
  state.translations.clear();
}

// The same prompt as LANTalk's OllamaClient.
const translationPrompt = (text, source, target) =>
  `You are a professional ${source} to ${target} translator.\n\n` +
  "Translate the message naturally, preserving the casual tone and all technical " +
  "identifiers, code, URLs, and product names exactly as written.\n\n" +
  "Output only the translation, without explanations.\n\n" +
  `Translate this message:\n\n${text}`;

async function ollamaGenerate(url, model, prompt) {
  // text/plain keeps this a simple request without a CORS preflight; Ollama
  // reads the JSON body either way.
  const response = await fetch(`${url.replace(/\/+$/, "")}/api/generate`, {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: JSON.stringify({ model, prompt, stream: false }),
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.error || `Ollama answered ${response.status}.`);
  if (!body.response) throw new Error("Ollama returned an invalid translation response.");
  return body.response.trim();
}

// Like MessageTranslator: quotes stay as they are.
async function translateDirectly(text, source, target, url, model) {
  if (!url || !model) throw new Error(t("chooseOllama"));
  const segments = parseQuotes(text);
  for (const segment of segments) {
    if (!segment.quote && segment.text.trim()) {
      segment.text = await ollamaGenerate(url, model, translationPrompt(segment.text, source, target));
    }
  }
  return segments.map(segment =>
    segment.quote ? `[quote]\n${segment.text}\n[/quote]` : segment.text).join("\n");
}

function sendTranslated(conversation, text) {
  const settings = translationSettings(conversation);
  const message = addMessage(conversation, { out: true, text, pending: true, translatingNow: true });
  translateMessage(conversation, text, settings.mine, settings.peer).then(translation => {
    message.translatingNow = false;
    message.translation = translation;
    const line = `MESSAGE ${encodeField(translation)} ORG_LANG ${encodeField(text)}`;
    const delivered = sendToConversation(conversation, line, () => {
      message.pending = false;
      saveHistory();
      refreshMessageElement(message);
    });
    if (delivered) refreshMessageElement(message);
  }, error => {
    message.translatingNow = false;
    message.pending = false;
    message.failed = true;
    refreshMessageElement(message);
    addNotice(conversation, t("translationFailed", error.message));
    saveHistory();
  });
}

function receiveTranslated(conversation, text) {
  const settings = translationSettings(conversation);
  const message = addMessage(conversation, { out: false, text, translatingNow: true });
  translateMessage(conversation, text, settings.peer, settings.mine).then(translation => {
    message.original = text;
    message.text = translation;
  }, error => {
    addNotice(conversation, t("incomingFailed", error.message));
  }).finally(() => {
    message.translatingNow = false;
    saveHistory();
    refreshMessageElement(message);
    renderPeople();
    announce(conversation, preview(message.text));
  });
}

// ---------------------------------------------------- translate sheet --

let directModels = { url: "", models: [], error: "" };

function openTranslateSheet() {
  const conversation = state.conversations.get(state.current);
  if (!conversation) return;
  if (conversation.translating || conversation.translateRequest) {
    stopTranslation(conversation);
    return;
  }
  const settings = translationSettings(conversation);
  for (const [select, value] of [[$("translate-mine"), settings.mine],
                                 [$("translate-peer"), settings.peer]]) {
    select.replaceChildren(...Object.keys(TRANSLATION_LANGUAGES).map(name => {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = languageLabel(name);
      return option;
    }));
    select.value = value;
  }
  $("translate-url").value = settings.source === "direct" ? settings.url : "";
  const hasFound = state.ollama.servers.some(server => server.models.length);
  const source = settings.source === "direct" || (!state.ollama.discovery && !hasFound)
    ? "direct" : "server";
  for (const radio of document.querySelectorAll('input[name="source"]')) {
    radio.checked = radio.value === source;
  }
  $("translate-sheet").dataset.server = settings.source === "server"
    ? JSON.stringify([settings.url, settings.model]) : "";
  $("translate-sheet").dataset.model = settings.source === "direct" ? settings.model : "";
  $("translate-sheet").hidden = false;
  history.pushState({ sheet: true }, "");
  if (!state.ollama.configured && state.ollama.discovery) sendCentral("CENTRAL_OLLAMA_SCAN");
  renderTranslateSheet(true);
}

function closeTranslateSheet() {
  $("translate-sheet").hidden = true;
}

function selectedSource() {
  const checked = document.querySelector('input[name="source"]:checked');
  return checked ? checked.value : "server";
}

function renderTranslateSheet(checkAddress) {
  const configured = state.ollama.configured;
  $("translate-by-server").hidden = !configured;
  $("translate-source").hidden = configured;
  // lantalkd checks its Ollama server every minute.
  const down = configured && state.ollama.available === false;
  $("translate-by-server").textContent = t(down ? "serverOllamaDown" : "serverTranslates");
  $("translate-start").disabled = down;
  if (configured) return;

  for (const body of document.querySelectorAll(".choice-body")) {
    body.hidden = body.dataset.for !== selectedSource();
  }

  // Servers lantalkd found, one entry per model.
  const select = $("translate-server");
  const wanted = select.value || $("translate-sheet").dataset.server;
  const options = [];
  for (const server of state.ollama.servers) {
    for (const model of server.models) {
      const option = document.createElement("option");
      option.value = JSON.stringify([server.url, model]);
      option.textContent = `${model} — ${server.url.replace(/^https?:\/\//, "")}`;
      options.push(option);
    }
  }
  select.replaceChildren(...options);
  if (options.some(option => option.value === wanted)) select.value = wanted;
  select.disabled = !options.length;
  const servers = state.ollama.servers.filter(server => server.models.length).length;
  $("translate-server-status").textContent =
    !state.ollama.discovery ? t("noDiscovery") :
    state.ollama.scanning ? t("scanning") :
    servers ? t("someFound", servers) : t("noneFound");
  $("translate-rescan").disabled = !state.ollama.discovery || state.ollama.scanning;

  if (checkAddress) checkDirectAddress();
  renderDirectModels();
}

async function checkDirectAddress() {
  const url = $("translate-url").value.trim().replace(/\/+$/, "");
  if (!url || url === directModels.url) return;
  directModels = { url, models: [], error: "", checking: true };
  renderDirectModels();
  try {
    const response = await fetch(`${url}/api/tags`);
    const body = await response.json();
    directModels = { url, models: (body.models || []).map(m => m.name || m.model).filter(Boolean),
                     error: "" };
  } catch {
    directModels = { url, models: [], error:
      location.protocol === "https:" && url.startsWith("http:")
        ? t("mixedContent") : t("unreachable", location.origin) };
  }
  if ($("translate-url").value.trim().replace(/\/+$/, "") === url) renderDirectModels();
}

function renderDirectModels() {
  const select = $("translate-model");
  const wanted = select.value || $("translate-sheet").dataset.model;
  select.replaceChildren(...directModels.models.map(model => {
    const option = document.createElement("option");
    option.value = model;
    option.textContent = model;
    return option;
  }));
  if (directModels.models.includes(wanted)) select.value = wanted;
  select.disabled = !directModels.models.length;
  const url = $("translate-url").value.trim();
  $("translate-direct-status").textContent =
    !url ? "" :
    directModels.checking ? t("checkingAddress") :
    directModels.error ? directModels.error :
    directModels.models.length ? t("modelsFound", directModels.models.length) : t("noModels");
}

function submitTranslateSheet() {
  const conversation = state.conversations.get(state.current);
  if (!conversation) return;
  const settings = translationSettings(conversation);
  settings.mine = $("translate-mine").value;
  settings.peer = $("translate-peer").value;
  if (!state.ollama.configured) {
    settings.source = selectedSource();
    if (settings.source === "server") {
      [settings.url, settings.model] = $("translate-server").value
        ? JSON.parse($("translate-server").value) : ["", ""];
    } else {
      settings.url = $("translate-url").value.trim().replace(/\/+$/, "");
      settings.model = $("translate-model").value;
    }
    if (!settings.url || !settings.model) {
      const status = settings.source === "server" ? $("translate-server-status")
                                                  : $("translate-direct-status");
      status.textContent = t("chooseOllama");
      return;
    }
  }
  settings.enabled = true;
  saveTranslationSettings(conversation, settings);
  history.back();
  requestTranslation(conversation, false);
}

function setupTranslation() {
  $("translate").onclick = openTranslateSheet;
  $("translate-cancel").onclick = () => history.back();
  $("translate-sheet").onclick = event => {
    if (event.target === $("translate-sheet")) history.back();
  };
  $("translate-form").onsubmit = event => {
    event.preventDefault();
    submitTranslateSheet();
  };
  $("translate-rescan").onclick = () => sendCentral("CENTRAL_OLLAMA_SCAN");
  for (const radio of document.querySelectorAll('input[name="source"]')) {
    radio.onchange = () => renderTranslateSheet(true);
  }
  let timer = 0;
  $("translate-url").oninput = () => {
    clearTimeout(timer);
    timer = setTimeout(checkDirectAddress, 600);
    renderDirectModels();
  };
  $("translate-url").onchange = checkDirectAddress;
}

// ------------------------------------------------------------ history --

// The last messages of each conversation stay on this device, per nickname.
const historyKey = () => `lantalk.history.${normalizeNickname(state.nickname)}`;
let saveTimer = 0;

function saveHistory() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    // When the browser's storage is full, keep fewer messages rather than
    // none.
    for (let limit = HISTORY_LIMIT; limit >= 50; limit = Math.floor(limit / 2)) {
      if (storage.set(historyKey(), JSON.stringify(historyData(limit)))) return;
    }
  }, 300);
}

function historyData(limit) {
  const data = [];
  for (const conversation of state.conversations.values()) {
    // Files live only as long as the page; history keeps their names.
    const messages = conversation.messages.slice(-limit).map(message =>
      message.file
        ? { out: message.out, time: message.time, notice: `📎 ${message.file.name}` }
        : { out: message.out, time: message.time, text: message.text,
            original: message.original || undefined,
            translation: message.translation || undefined, notice: message.notice,
            // A message still being translated never went out.
            pending: (message.pending && !message.translatingNow) || undefined,
            failed: message.failed || (message.out && message.translatingNow) || undefined });
    if (messages.length) {
      data.push({ nickname: conversation.nickname, address: conversation.address, messages });
    }
  }
  return data;
}

function loadHistory() {
  state.conversations.clear();
  let data = [];
  try { data = JSON.parse(storage.get(historyKey()) || "[]"); } catch { data = []; }
  for (const entry of Array.isArray(data) ? data : []) {
    if (!entry || !entry.nickname || !Array.isArray(entry.messages)) continue;
    const conversation = newConversation(conversationKey(entry.address, entry.nickname),
                                         entry.nickname, entry.address);
    conversation.messages = entry.messages.filter(m => m && (m.text || m.notice));
    // Messages that never went out are sent when the person is reachable.
    for (const message of conversation.messages) {
      if (!message.pending || !message.text) continue;
      const line = message.translation
        ? `MESSAGE ${encodeField(message.translation)} ORG_LANG ${encodeField(message.text)}`
        : `MESSAGE ${encodeField(message.text)}`;
      conversation.queue.push({ line, onSent: () => {
        message.pending = false;
        saveHistory();
        refreshMessageElement(message);
      } });
    }
    conversation.lastActivity = conversation.messages.length
      ? conversation.messages[conversation.messages.length - 1].time || 0 : 0;
  }
}

// ------------------------------------------------------ notifications --

let audio = null;

// A soft rising two-note chime, like LANTalk's own.
function chime() {
  try {
    audio = audio || new (window.AudioContext || window.webkitAudioContext)();
    if (audio.state === "suspended") audio.resume();
    const now = audio.currentTime;
    for (const [frequency, delay, level] of [[1318.51, 0, 0.18], [1760, 0.09, 0.15]]) {
      const oscillator = audio.createOscillator();
      const gain = audio.createGain();
      oscillator.type = "sine";
      oscillator.frequency.value = frequency;
      gain.gain.setValueAtTime(0, now + delay);
      gain.gain.linearRampToValueAtTime(level, now + delay + 0.006);
      gain.gain.exponentialRampToValueAtTime(0.0001, now + delay + 0.5);
      oscillator.connect(gain).connect(audio.destination);
      oscillator.start(now + delay);
      oscillator.stop(now + delay + 0.55);
    }
  } catch { /* no audio */ }
}

// Browsers only allow sound after a tap; unlock it on the first one.
document.addEventListener("pointerdown", () => {
  try {
    audio = audio || new (window.AudioContext || window.webkitAudioContext)();
    if (audio.state === "suspended") audio.resume();
  } catch { /* no audio */ }
}, { once: true });

let lastChime = 0;

function announce(conversation, text) {
  if (isVisible(conversation)) return;
  if (Date.now() - lastChime > 1500) {
    lastChime = Date.now();
    chime();
    if (navigator.vibrate) navigator.vibrate(80);
  }
  if (document.hidden && "Notification" in window && Notification.permission === "granted") {
    try {
      const notification = new Notification(conversation.nickname,
                                            { body: text, icon: "icon.svg", tag: conversation.key });
      notification.onclick = () => { window.focus(); openConversation(conversation.key); };
    } catch { /* not allowed here */ }
  }
}

function isVisible(conversation) {
  return !document.hidden && state.current === conversation.key &&
         (document.body.classList.contains("in-chat") || matchMedia("(min-width: 760px)").matches);
}

function updateTitle() {
  let unread = 0;
  for (const conversation of state.conversations.values()) unread += conversation.unread;
  document.title = unread ? `(${unread}) LANTalk` : "LANTalk";
  if (navigator.setAppBadge) {
    (unread ? navigator.setAppBadge(unread) : navigator.clearAppBadge()).catch(() => {});
  }
}

// ----------------------------------------------------------- rendering --

const AVATAR_COLORS = ["#17bcae", "#8069e8", "#0b84fe", "#e8578a", "#f08c2e",
                       "#2fa84f", "#c94fd6", "#d9a400", "#4f7be8", "#e0544b"];

function avatar(element, nickname, online, away = false) {
  let hash = 0;
  for (const character of normalizeNickname(nickname)) {
    hash = (hash * 31 + character.codePointAt(0)) >>> 0;
  }
  element.style.background = AVATAR_COLORS[hash % AVATAR_COLORS.length];
  element.textContent = Array.from(nickname.trim())[0] || "?";
  element.classList.toggle("online", online);
  element.classList.toggle("away", online && away);
}

// The peer says so on the conversation, or lantalkd does.
function isAway(peer, conversation) {
  return Boolean((peer && peer.away) || (conversation && conversation.peerAway));
}

function showBanner(text) {
  const banner = $("connection");
  banner.textContent = text || "";
  banner.hidden = !text;
}

function renderPeople() {
  if (!state.ready && !state.nickname) return;
  const filter = normalizeNickname($("search").value);
  const list = $("people-list");
  const entries = [];
  const seen = new Set();
  for (const peer of state.peers.values()) {
    const key = peerKey(peer);
    if (seen.has(key)) continue;
    seen.add(key);
    entries.push({ key, nickname: peer.nickname, address: peer.address, online: true,
                   away: isAway(peer, state.conversations.get(key)),
                   conversation: state.conversations.get(key) });
  }
  for (const conversation of state.conversations.values()) {
    if (seen.has(conversation.key) || !conversation.messages.length) continue;
    entries.push({ key: conversation.key, nickname: conversation.nickname,
                   address: conversation.address, online: false, conversation });
  }
  const filtered = entries.filter(entry => !filter ||
    normalizeNickname(entry.nickname).includes(filter) || entry.address.includes(filter));
  filtered.sort((a, b) =>
    ((b.conversation && b.conversation.lastActivity) || 0) -
      ((a.conversation && a.conversation.lastActivity) || 0) ||
    (b.online - a.online) ||
    a.nickname.localeCompare(b.nickname));

  // Same-named people at different addresses show their address.
  const nameCount = new Map();
  for (const entry of entries) {
    const name = normalizeNickname(entry.nickname);
    nameCount.set(name, (nameCount.get(name) || 0) + 1);
  }

  list.replaceChildren(...filtered.map(entry => {
    const item = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "person";
    button.classList.toggle("offline", !entry.online);
    button.classList.toggle("current", entry.key === state.current);
    button.onclick = () => openConversation(entry.key, entry);

    const face = document.createElement("div");
    face.className = "avatar";
    avatar(face, entry.nickname, entry.online, entry.away);

    const text = document.createElement("div");
    text.className = "person-text";
    const name = document.createElement("div");
    name.className = "person-name";
    const nameText = document.createElement("span");
    nameText.textContent = entry.nickname;
    name.append(nameText);
    if (nameCount.get(normalizeNickname(entry.nickname)) > 1) {
      const address = document.createElement("span");
      address.className = "person-address";
      address.textContent = entry.address;
      name.append(address);
    }
    const line = document.createElement("div");
    line.className = "person-preview";
    const conversation = entry.conversation;
    const last = conversation && [...conversation.messages].reverse().find(m => m.text || m.file);
    if (conversation && conversation.peerTyping) {
      line.classList.add("typing");
      line.textContent = t("typingShort");
    } else if (last) {
      const body = last.file ? `📎 ${last.file.name}` : preview(last.text);
      line.textContent = last.out ? `${t("you")}: ${body}` : body;
    } else {
      line.textContent = !entry.online ? t("offline") : entry.away ? t("away") : t("online");
    }
    text.append(name, line);
    button.append(face, text);
    if (conversation && conversation.unread) {
      const badge = document.createElement("span");
      badge.className = "badge";
      badge.textContent = conversation.unread > 99 ? "99+" : String(conversation.unread);
      button.append(badge);
    }
    item.append(button);
    return item;
  }));

  const empty = $("people-empty");
  empty.hidden = filtered.length > 0;
  empty.textContent = filter ? t("noMatches", $("search").value.trim()) : t("nobodyOnline");
}

function openConversation(key, entry) {
  let conversation = state.conversations.get(key);
  if (!conversation && entry) conversation = newConversation(key, entry.nickname, entry.address);
  if (!conversation) return;
  if (state.current && state.current !== key) {
    const previous = state.conversations.get(state.current);
    if (previous) stopLocalTyping(previous);
  }
  state.current = key;
  conversation.unread = 0;
  if (!document.body.classList.contains("in-chat")) {
    document.body.classList.add("in-chat");
    history.pushState({ chat: key }, "");
  }
  $("input").value = storage.get(`lantalk.draft.${key}`, true) || "";
  ensureChannel(conversation);
  renderChat();
  renderPeople();
  updateTitle();
  if (matchMedia("(min-width: 760px)").matches) $("input").focus();
}

function closeConversation() {
  const conversation = state.conversations.get(state.current);
  if (conversation) stopLocalTyping(conversation);
  document.body.classList.remove("in-chat");
  if (!matchMedia("(min-width: 760px)").matches) state.current = null;
  renderPeople();
}

function renderChatHeader() {
  const conversation = state.conversations.get(state.current);
  if (!conversation) return;
  const peer = peerFor(conversation);
  const online = Boolean(peer);
  const away = isAway(peer, conversation);
  $("chat-name").textContent = conversation.nickname;
  avatar($("chat-avatar"), conversation.nickname, online, away);
  const channel = currentChannel(conversation);
  $("chat-status").textContent =
    !state.ready ? t("connecting") :
    !online ? t("offline") :
    channel && !channel.handshake ? t("connecting") :
    `${t(away ? "away" : "online")} · ${peer.account && peer.devices > 1 ? t("devices", peer.devices)
                                        : peer.web ? "Web" : peer.address}`;
}

function renderChat() {
  const conversation = state.conversations.get(state.current);
  renderTranslateButton();
  $("chat-placeholder").hidden = Boolean(conversation);
  $("composer").hidden = !conversation;
  $("messages").hidden = !conversation;
  if (!conversation) return;
  renderChatHeader();
  const messages = $("messages");
  messages.replaceChildren();
  lastRendered = null;
  for (const message of conversation.messages) appendMessageElement(message, false);
  renderTyping();
  updateSendButton();
  autoGrow();
  requestAnimationFrame(() => { messages.scrollTop = messages.scrollHeight; });
}

let lastRendered = null;
const elements = new WeakMap();

function dayLabel(time) {
  const date = new Date(time);
  const today = new Date();
  const yesterday = new Date(Date.now() - 86400000);
  if (date.toDateString() === today.toDateString()) return t("today");
  if (date.toDateString() === yesterday.toDateString()) return t("yesterday");
  return date.toLocaleDateString(language.replace("_", "-"),
                                 { weekday: "short", day: "numeric", month: "short" });
}

function appendMessageElement(message, scroll) {
  const container = $("messages");
  const nearBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 120;
  if (!lastRendered || new Date(lastRendered.time).toDateString() !==
                       new Date(message.time).toDateString()) {
    const day = document.createElement("div");
    day.className = "day";
    day.textContent = dayLabel(message.time);
    container.append(day);
  }
  let element;
  if (message.notice) {
    element = document.createElement("div");
    element.className = "notice";
    element.textContent = message.notice;
  } else {
    element = document.createElement("div");
    element.className = "message";
    element.classList.toggle("out", message.out);
    if (lastRendered && !lastRendered.notice && lastRendered.out === message.out &&
        message.time - lastRendered.time < 60000) {
      element.classList.add("follow");
    }
    fillMessage(element, message);
  }
  elements.set(message, element);
  container.append(element);
  lastRendered = message;
  if (scroll && (nearBottom || message.out)) container.scrollTop = container.scrollHeight;
}

function refreshMessageElement(message) {
  const element = elements.get(message);
  if (element && element.isConnected) fillMessage(element, message);
}

function fillMessage(element, message) {
  element.replaceChildren();
  if (message.transfer) {
    const { name, peer, percent, failed } = message.transfer;
    const bubble = document.createElement("div");
    bubble.className = "bubble pending";
    bubble.classList.toggle("failed", Boolean(failed));
    bubble.textContent = failed
      ? t(message.out ? "sendFailed" : "receiveFailed", name, peer)
      : t(message.out ? "uploading" : "receiving", name, peer, percent);
    element.append(bubble);
  } else if (message.file) {
    const isImage = message.file.mime.startsWith("image/");
    if (isImage) {
      const wrapper = document.createElement("div");
      wrapper.className = "attachment";
      const image = document.createElement("img");
      image.src = message.file.url;
      image.alt = message.file.name;
      image.loading = "lazy";
      image.onclick = () => showImage(message.file.url, message.file.name);
      wrapper.append(image);
      element.append(wrapper);
    }
    if (!isImage) {
      const bubble = document.createElement("div");
      bubble.className = "bubble file";
      bubble.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/></svg>';
      const link = document.createElement("a");
      link.href = message.file.url;
      link.download = message.file.name;
      link.textContent = message.file.name;
      bubble.append(link);
      element.append(bubble);
    }
  } else {
    const bubble = document.createElement("div");
    bubble.className = "bubble";
    bubble.classList.toggle("pending", Boolean(message.pending || message.translatingNow));
    bubble.classList.toggle("failed", Boolean(message.failed));
    for (const segment of parseQuotes(message.text)) {
      if (segment.quote) {
        const quote = document.createElement("div");
        quote.className = "quote";
        quote.textContent = segment.text;
        bubble.append(quote);
      } else {
        const span = document.createElement("span");
        linkify(span, segment.text);
        bubble.append(span);
      }
    }
    element.append(bubble);
  }

  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = new Date(message.time).toLocaleTimeString(language.replace("_", "-"),
                                                              { hour: "2-digit", minute: "2-digit" });
  if (message.pending && !message.file) meta.textContent += " · …";
  // A translated message can show its other version: the original of a
  // received one, the sent translation of our own.
  const other = message.original || message.translation;
  if (other) {
    const received = Boolean(message.original);
    const toggle = document.createElement("button");
    toggle.type = "button";
    toggle.textContent = message.showOther
      ? t(received ? "hideOriginal" : "hideTranslation")
      : t(received ? "original" : "sentTranslation");
    toggle.onclick = () => {
      message.showOther = !message.showOther;
      fillMessage(element, message);
    };
    meta.append(toggle);
  }
  if (message.translatingNow) meta.textContent += ` · ${t("translating")}`;
  element.append(meta);
  if (other && message.showOther) {
    const extra = document.createElement("div");
    extra.className = "original";
    extra.textContent = other;
    element.append(extra);
  }
}

function linkify(element, text) {
  const pattern = /\bhttps?:\/\/[^\s<>"]+[^\s<>".,;:!?)\]]/g;
  let position = 0;
  for (const match of text.matchAll(pattern)) {
    element.append(text.slice(position, match.index));
    const link = document.createElement("a");
    link.href = match[0];
    link.textContent = match[0];
    link.target = "_blank";
    link.rel = "noopener noreferrer";
    element.append(link);
    position = match.index + match[0].length;
  }
  element.append(text.slice(position));
}

function renderTyping() {
  const conversation = state.conversations.get(state.current);
  const typing = $("typing");
  typing.hidden = !conversation || !conversation.peerTyping;
  if (conversation) typing.textContent = t("typing", conversation.nickname);
}

function showImage(url, name) {
  $("viewer-image").src = url;
  $("viewer-image").alt = name;
  $("viewer").hidden = false;
  history.pushState({ viewer: true }, "");
}

function hideImage() {
  $("viewer").hidden = true;
  $("viewer-image").removeAttribute("src");
}

// -------------------------------------------------------------- input --

function autoGrow() {
  const input = $("input");
  input.style.height = "auto";
  input.style.height = `${input.scrollHeight + 2}px`;
}

function updateSendButton() {
  $("send").disabled = !$("input").value.trim();
}

function submitMessage() {
  const conversation = state.conversations.get(state.current);
  const input = $("input");
  const text = input.value.replace(/^(\s*\n)+/, "").replace(/\s+$/, "");
  if (!conversation || !text) return;
  input.value = "";
  storage.set(`lantalk.draft.${conversation.key}`, null, true);
  stopLocalTyping(conversation);
  sendText(conversation, text);
  updateSendButton();
  autoGrow();
}

// Tells lantalkd and every conversation that supports it.
function setAway(away) {
  state.away = away;
  sendCentral(`CENTRAL_STATUS ${away ? "away" : "active"}`);
  for (const channel of state.channels.values()) {
    if (channel.handshake && channel.presence) {
      sendChat(channel, `PRESENCE ${away ? "AWAY" : "ACTIVE"}`);
    }
  }
}

// ------------------------------------------------------------- sign-in --

function showLogin(error) {
  document.body.classList.remove("in-chat");
  $("app").hidden = true;
  $("login").hidden = false;
  $("login-nickname").value = state.nickname || storage.get("lantalk.nickname") || "";
  $("login-error").hidden = !error;
  $("login-error").textContent = error || "";
  $("login-submit").disabled = false;
  $("login-submit").textContent = t("signIn");
  (!$("login-pin-field").hidden ? $("login-pin")
    : state.nickname ? $("login-password") : $("login-nickname")).focus();
}

// Only a refused password is forgotten: an unreachable server or a sign-in
// elsewhere (perhaps in another tab sharing this storage) says nothing
// about it.
function failSignIn(error, forgetPassword = false, askForPin = false) {
  state.signingIn = false;
  disconnect();
  $("login-pin-field").hidden = !askForPin;
  $("login-pin").value = "";
  if (forgetPassword) {
    storage.set("lantalk.password", null);
    storage.set("lantalk.password", null, true);
  }
  showLogin(error);
}

// A remembered sign-in keeps trying while the server is away; one the user
// just typed reports the failure.
function startSignIn(nickname, password, automatic = false) {
  state.nickname = nickname;
  state.password = password;
  state.signingIn = !automatic;
  loadHistory();
  connect();
}

function signedIn() {
  state.signingIn = false;
  state.pinOffered = state.pinOffered && state.nickname === state.lastNickname;
  state.lastNickname = state.nickname;
  $("login-pin-field").hidden = true;
  showBanner("");
  $("login").hidden = true;
  $("app").hidden = false;
  $("me").textContent = state.myAddress ? `${state.nickname} · ${state.myAddress}` : state.nickname;
  renderPeople();
  const conversation = state.conversations.get(state.current);
  if (conversation) {
    renderChat();
    ensureChannel(conversation);
  } else {
    renderChat();
  }
  renderPinButton();
  offerPin();
}

function signOut() {
  disconnect();
  for (const conversation of state.conversations.values()) {
    clearTimeout(conversation.peerTypingTimer);
    stopLocalTyping(conversation);
  }
  state.current = null;
  storage.set("lantalk.password", null);
  storage.set("lantalk.password", null, true);
  state.password = "";
  showLogin("");
}

// --------------------------------------------------------------- setup --

function setup() {
  const select = $("language");
  for (const [code, flag, name] of LANGUAGES) {
    const option = document.createElement("option");
    option.value = code;
    option.textContent = `${flag} ${name}`;
    select.append(option);
  }
  select.value = language;
  select.onchange = () => {
    language = select.value;
    storage.set("lantalk.language", language);
    applyTexts();
  };
  applyTexts();

  $("login-form").onsubmit = event => {
    event.preventDefault();
    const nickname = $("login-nickname").value.replace(/\s+/g, " ").trim();
    if (!nickname) return;
    const password = $("login-password").value;
    const remember = $("login-remember").checked;
    if (!$("login-pin-field").hidden) {
      const pin = $("login-pin").value;
      if (!isPin(pin)) {
        $("login-error").textContent = t("pinFourDigits");
        $("login-error").hidden = false;
        return;
      }
      state.nickname = nickname;
      storage.set(pinKey(), pinVerifier(pin));
    }
    storage.set("lantalk.nickname", nickname);
    storage.set("lantalk.password", remember ? password : null);
    storage.set("lantalk.password", password, true);
    $("login-password").value = "";
    $("login-error").hidden = true;
    $("login-submit").disabled = true;
    $("login-submit").textContent = t("signingIn");
    startSignIn(nickname, password);
  };

  $("sign-out").onclick = signOut;
  $("pin-button").onclick = openPinSheet;
  $("pin-later").onclick = () => closePinSheet(true);
  $("pin-form").onsubmit = event => {
    event.preventDefault();
    submitPin();
  };
  setupTranslation();
  $("back").onclick = () => history.back();
  $("search").oninput = renderPeople;
  $("viewer").onclick = () => history.back();

  window.addEventListener("popstate", () => {
    if (!$("viewer").hidden) { hideImage(); return; }
    if (!$("translate-sheet").hidden) { closeTranslateSheet(); return; }
    if (document.body.classList.contains("in-chat")) closeConversation();
  });

  const input = $("input");
  input.addEventListener("input", () => {
    autoGrow();
    updateSendButton();
    const conversation = state.conversations.get(state.current);
    if (!conversation) return;
    storage.set(`lantalk.draft.${conversation.key}`, input.value || null, true);
    noteLocalTyping(conversation, input.value.trim().length > 0);
  });
  input.addEventListener("keydown", event => {
    // On phones Enter makes a new line and the button sends; with a
    // keyboard, Enter sends and Shift+Enter makes a new line.
    if (event.key === "Enter" && !event.shiftKey && !event.isComposing &&
        !matchMedia("(pointer: coarse)").matches) {
      event.preventDefault();
      submitMessage();
    }
  });
  $("composer").onsubmit = event => {
    event.preventDefault();
    submitMessage();
    // Keep the keyboard up on phones.
    input.focus();
  };
  $("file").onchange = () => {
    const conversation = state.conversations.get(state.current);
    if (conversation) for (const file of $("file").files) sendFile(conversation, file);
    $("file").value = "";
  };

  // Away after AWAY_AFTER_MS without input, back at the first touch.
  const noteActivity = () => {
    state.lastActivity = Date.now();
    if (state.away) setAway(false);
  };
  for (const type of ["pointerdown", "keydown", "touchstart", "wheel"]) {
    document.addEventListener(type, noteActivity, { passive: true });
  }
  let lastMove = 0;
  document.addEventListener("mousemove", () => {
    if (Date.now() - lastMove > 5000) { lastMove = Date.now(); noteActivity(); }
  }, { passive: true });
  setInterval(() => {
    if (!state.away && Date.now() - state.lastActivity > AWAY_AFTER_MS) setAway(true);
  }, 30 * 1000);

  document.addEventListener("visibilitychange", () => {
    if (document.hidden) return;
    noteActivity();
    // Phones suspend background pages; come back at once, and make sure a
    // connection that looks open still answers.
    if (!state.stopped && !state.socket) connect();
    if (state.ready) {
      const asked = Date.now();
      sendCentral("CENTRAL_PING");
      setTimeout(() => {
        if (state.ready && state.lastReceived < asked) dropConnection();
      }, WAKE_CHECK_MS);
    }
    const conversation = state.conversations.get(state.current);
    if (conversation && isVisible(conversation) && conversation.unread) {
      conversation.unread = 0;
      renderPeople();
      updateTitle();
    }
  });
  window.addEventListener("online", () => { if (!state.stopped && !state.socket) connect(); });

  const nickname = storage.get("lantalk.nickname");
  const password = storage.get("lantalk.password", true) ?? storage.get("lantalk.password");
  if (nickname && password !== null) {
    $("app").hidden = false;
    showBanner(t("connecting"));
    state.nickname = nickname;
    renderChat();
    startSignIn(nickname, password, true);
  } else {
    showLogin("");
  }
}

setup();
