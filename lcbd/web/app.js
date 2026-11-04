'use strict';

const POLL_MS = 200;
const SLOW_POLL_EVERY = 5; // History and log are fetched at every 5th poll
const TOAST_OK_MS = 5000;
const TOAST_ERROR_MS = 10000;
const TOAST_FADE_MS = 250;
const TOAST_MAX = 4;
const IO_CHANNELS = 16;
const UNITS = [0, 1]; // Rear, front
const SERVICE_USER = 'service';

const COLOR = {
  ink: '#1d232b', muted: '#687384', faint: '#a3abb7', grid: '#e1e4e8', weakPoint: '#b0b7c3',
  die: '#d5dae1', punch: '#e6e9ee', labelBackground: 'rgba(251, 252, 253, .9)',
  blue: '#1f6feb', orange: '#d97706', darkOrange: '#b45f00', green: '#1a7f37', purple: '#8250df', red: '#d1242f',
};
const font = (px, bold) => `${bold ? '600 ' : ''}${px}px system-ui, sans-serif`;

// [English, German, machine label]
const SPARE = ['-', '-', ''];
const INPUTS = [
  ['backgauge in swivel zone (no cylinder up)', 'Anschlag im Schwenkbereich (Zylinder nicht hoch)', '„Anschlag im SB“'],
  ['front unit at reference (0)', 'Wagen vorne in Referenz (0)', '„WMS Sensor vorne eingeschwenkt“'],
  ['rear unit at reference (0)', 'Wagen hinten in Referenz (0)', '„WMS Sensor hinten eingeschwenkt“'],
  ['cylinder down', 'Zylinder unten', '„WMS hinten abgesenkt“'],
  ['button reference', 'Taster Justieren', '„WMS justieren“'],
  ['button cylinder', 'Taster Zylinder', '„WMS aktivieren/absenken“'],
  ['die shift', 'Matrizenverschiebung', '„MV Bolzen ausgefahren“'],
  ['CNC: measuring system active', 'CNC: Messsystem aktiv', '„Hilfsfunktion WMS aktiv“'],
  ['CNC: measuring system inactive', 'CNC: Messsystem inaktiv', '„Hilfsfunktion WMS inaktiv“'],
  ['count pulses front', 'Zählimpulse vorne', '„WMS-Platine vorne SA“'],
  SPARE, SPARE, SPARE,
  ['count pulses rear', 'Zählimpulse hinten', '„WMS-Platine hinten SA“'],
  SPARE, SPARE,
];
const OUTPUTS = [
  ['front unit forward', 'Wagen vorne vor', '„WMS vorne Ab“'],
  ['front unit back', 'Wagen vorne zurück', '„WMS vorne Auf“'],
  ['rear unit forward', 'Wagen hinten vor', '„WMS hinten Ab“'],
  ['rear unit back', 'Wagen hinten zurück', '„WMS hinten Auf“'],
  ['cylinder down', 'Zylinder ab', '„WMS hinten Absenken“'],
  ['cylinder up', 'Zylinder hoch', '„WMS hinten aktivieren“'],
  ['lamp rear', 'Lampe hinten', '„WMS hinten Winkel gefunden“'],
  ['lamp front', 'Lampe vorne', '„WMS vorne Winkel gefunden“'],
].concat(Array(8).fill(SPARE)); // A9 to A16

// German texts of the labels, keyed by the English text
const DE = {
  'simulation': 'Simulation', 'service password': 'Service-Passwort',
  'I/O': 'E/A', 'CNC link': 'CNC-Verbindung', 'Controller': 'Steuerung', 'Sensors': 'Sensoren',
  'Die positions': 'Matrizenpositionen', 'Angle history': 'Winkelverlauf', 'Setup': 'Einstellungen', 'Log': 'Protokoll',
  'enable switching and driving': 'Schalten und Fahren freigeben', 'Inputs': 'Eingänge', 'Outputs': 'Ausgänge',
  'Enabled: outputs, counter reset and driving act directly on the machine.': 'Freigegeben: Ausgänge, Zähler nullen und Fahren wirken direkt auf die Maschine.',
  'enter the service password first': 'erst das Service-Passwort eingeben',
  'wrong or missing service password': 'Service-Passwort fehlt oder ist falsch',
  'refused: switching is not enabled on the page': 'abgelehnt: Schalten ist auf der Seite nicht freigegeben',
  'read only: no web.pass': 'nur lesen: keine web.pass vorhanden',
  'Rear unit (counter 1)': 'Wagen hinten (Zähler 1)', 'Front unit (counter 2)': 'Wagen vorne (Zähler 2)',
  'position': 'Position', 'raw counter': 'Zähler roh', 'drive': 'Antrieb', 'target': 'Ziel',
  'reset counter': 'Zähler nullen', 'drive to': 'fahren auf', 'start': 'Start', 'on': 'ein', 'off': 'aus',
  'device': 'Schnittstelle', 'CNC contact': 'Kontakt zur CNC', 'modem lines': 'Modemleitungen',
  'line supervision': 'Leitungsüberwachung', 'last byte received': 'letztes Byte empfangen',
  'last byte sent': 'letztes Byte gesendet', 'last command / group': 'letzter Befehl / Gruppe', 'parser': 'Parser',
  'parameter block / byte': 'Parameterblock / Byte', 'bytes received': 'Bytes empfangen', 'phase p': 'Phase p',
  'set angle': 'Sollwinkel', 'station angle sent': 'gesendeter Winkel', 'raw value': 'Rohwert',
  'referenced / encoder used / leg check': 'referenziert / Geber benutzt / Schenkelprüfung',
  'DNC messages': 'DNC-Meldungen', 'errors (85/81 replies)': 'Fehler (Antworten 85/81)',
  'springback queries': 'Rückfederungsabfragen', 'DNC message': 'DNC-Meldung',
  'no DNC message received': 'keine DNC-Meldung empfangen', 'open': 'offen', 'not open': 'nicht offen',
  'not available': 'nicht vorhanden', 'yes': 'ja', 'no': 'nein',
  'program version': 'Programmversion', 'uptime': 'Laufzeit', 'error': 'Fehler', 'controller cycle': 'Steuerungszyklus',
  'I/O cycle (last / max)': 'E/A-Zyklus (letzter / max.)', 'controller state': 'Zustand Steuerung',
  'reference sequence': 'Ablauf Referenz', 'cylinder sequence': 'Ablauf Zylinder',
  'die request sequence': 'Ablauf Matrizenabfrage', 'cylinder up': 'Zylinder oben', 'referenced': 'referenziert',
  'not referenced': 'nicht referenziert', 'scanning': 'Messung läuft', 'encoder used': 'Geber benutzt',
  'leg check': 'Schenkelprüfung', 'control-angle / measure-type / measure-this-bend': 'Regelwinkel / Messart / diese Biegung messen',
  'dies in the controller': 'Matrizen in der Steuerung', 'active setup': 'aktive Einstellung',
  'move down at start': 'beim Start absenken', 'secure angle': 'sicherer Winkel', 'use turn': 'Wenden',
  'B (both valid once)': 'B (beide einmal gültig)', 'valid rear / front': 'gültig hinten / vorne',
  'rear / front angle': 'Winkel hinten / vorne', 'combined angle': 'Gesamtwinkel', 'held angle': 'gehaltener Winkel',
  'pairs': 'Paare', 'Rear sensor': 'Sensor hinten', 'Front sensor': 'Sensor vorne', 'address': 'Adresse',
  'connected': 'verbunden', 'frames (valid / invalid)': 'Bilder (gültig / ungültig)', 'link phase': 'Phase',
  'leg angle α': 'Schenkelwinkel α', 'die angle δ': 'Matrizenwinkel δ', 'reference point R': 'Referenzpunkt R',
  'reference done': 'Referenz fertig', 'candidate points': 'Kandidatenpunkte',
  'good reference frames g': 'gute Referenzbilder g', 'die fits': 'Matrizenanpassungen', 'die points': 'Matrizenpunkte',
  'leg points n_P': 'Schenkelpunkte n_P', 'filtered L': 'gefiltert L', 'reference frame': 'Referenzbild',
  'sections 1–3': 'Abschnitte 1–3', 'full line': 'ganze Gerade', 'leg line λ': 'Schenkelgerade λ', 'valid': 'gültig',
  'no laser points in this frame': 'keine Laserpunkte in diesem Bild', 'laser points': 'Laserpunkte',
  'die flank': 'Matrizenflanke', 'leg line': 'Schenkelgerade', 'bend angle': 'Biegewinkel',
  'not measured': 'nicht gemessen', 'held': 'gehalten',
  'name': 'Name', 'rear': 'hinten', 'front': 'vorne', 'add row': 'Zeile hinzufügen',
  'take current positions': 'aktuelle Positionen übernehmen', 'delete row': 'Zeile löschen', 'save': 'speichern',
  'send to controller': 'an Steuerung senden', 'request list from the CNC': 'Liste von der CNC anfordern',
  'select a row first': 'erst eine Zeile wählen', 'combined': 'gesamt', 'phase (× 10)': 'Phase (× 10)', 'last': 'letzte',
  'General': 'Allgemein', 'Springback': 'Rückfederung', 'Line detection': 'Geradenerkennung',
  'move method': 'Verfahrmethode', 'max. measuring time in s': 'max. Messzeit in s',
  'averaging, number of tilt angles': 'Mittelung, Anzahl Kippwinkel', 'line mode': 'Geradenmodus',
  'fixed number of points line 3': 'feste Punktzahl Abschnitt 3', 'minimum number of points': 'Mindestpunktzahl',
  'maximum overlap in %': 'max. Überlappung in %', 'tolerance in degrees': 'Toleranz in Grad',
  'Cancel': 'Abbrechen', 'active': 'aktiv', 'phase': 'Phase', 'angle': 'Winkel', 'cylinder': 'Zylinder',
  'up': 'oben', 'down': 'unten', 'request failed': 'Anfrage fehlgeschlagen',
};
// German texts of the state values sent by lcbd
const DE_VALUES = {
  'start': 'Start', 'idle': 'bereit', 'waiting for reference': 'wartet auf Referenz',
  'waiting for cylinder': 'wartet auf Zylinder', 'waiting for die request': 'wartet auf Matrizenabfrage',
  'retry delay': 'Wartezeit vor Wiederholung', 'no error': 'kein Fehler', 'cylinder': 'Zylinder',
  'reference': 'Referenz', 'die request': 'Matrizenabfrage', 'unknown': 'unbekannt', 'home': 'Heimfahrt',
  'full reference': 'volle Referenz', 'static reference': 'statische Referenz', 'cylinder up': 'Zylinder hoch',
  'cylinder down': 'Zylinder ab', 'cylinder (invalid)': 'Zylinder (ungültig)', 'die list': 'Matrizenliste',
  'off': 'aus', 'forward': 'vor', 'back': 'zurück', 'positioning': 'positionieren', 'coasting': 'Auslauf',
  'correction': 'Korrektur', 'in position': 'in Position', 'timeout': 'Zeitüberschreitung',
  'CTS/DSR inactive': 'CTS/DSR inaktiv', 'command': 'Befehl', 'parameter block': 'Parameterblock',
  'DNC header': 'DNC-Kopf', 'DNC data': 'DNC-Daten',
};
// Tooltips: [English, German]
const TIPS = {
  password: ['Needed for switching and saving. Viewing works without a password.',
    'Nötig zum Schalten und Speichern. Ansehen geht ohne Passwort.'],
  enable: ['Unlocks the output buttons, counter reset and driving. They act directly on the machine: only with the press at the top and nobody in the swivel area. Needs the service password.',
    'Gibt die Ausgangstasten, Zähler nullen und Fahren frei. Sie wirken direkt auf die Maschine: nur mit der Presse oben und niemandem im Schwenkbereich. Braucht das Service-Passwort.'],
  reset: ['Sets the position counter of this unit to 0.', 'Setzt den Positionszähler dieses Wagens auf 0.'],
  drive: ['Drives the unit to the given position (in pulses).', 'Fährt den Wagen auf die angegebene Position (in Impulsen).'],
  die_take: ['Copies the current counters of both units into the selected row.',
    'Übernimmt die aktuellen Zählerstände beider Wagen in die gewählte Zeile.'],
  die_save: ['Writes the table to die.dat.', 'Schreibt die Tabelle in die.dat.'],
  die_send: ['Hands the table to the controller. Only used with move method MV_POS.',
    'Gibt die Tabelle an die Steuerung. Nur bei Verfahrmethode MV_POS benutzt.'],
  die_list: ['Asks the CNC for its die list.', 'Fragt die Matrizenliste bei der CNC ab.'],
  MV_METHOD: ['What happens on a die shift (E7). MV_REF: the units go home and reference again at the new die. MV_POS: the units drive to the positions stored for the die (needs DNC).',
    'Verhalten bei Matrizenverschiebung (E7). MV_REF: Die Wagen fahren heim und referenzieren neu an der Matrize. MV_POS: Die Wagen fahren auf die gespeicherten Positionen der Matrize (braucht DNC).'],
  MOVE_DOWN_START: ['When lcbd starts, both units drive home and the cylinder is lowered.',
    'Beim Start von lcbd fahren beide Wagen heim, danach wird der Zylinder abgesenkt.'],
  USE_SECURE_ANGLE: ['While a leg cannot be measured, the CNC gets "angle invalid" (81) instead of an angle.',
    'Solange ein Schenkel nicht messbar ist, bekommt die CNC „Winkel ungültig“ (81) statt eines Winkels.'],
  SB_TIMEOUT: ['Springback: longest measuring time in seconds. No effect at this machine, the CNC does not ask for springback.',
    'Rückfederung: längste Messzeit in Sekunden. An dieser Maschine ohne Wirkung, die CNC fragt keine Rückfederung ab.'],
  SB_MEANTILT: ['Springback: number of tilt angles that are averaged. No effect at this machine.',
    'Rückfederung: Anzahl der gemittelten Kippwinkel. An dieser Maschine ohne Wirkung.'],
  LE_LINE_MODE: ['LM_EXTENDED: the leg line is fitted in up to three overlapping sections that must agree. LM_STANDARD: one straight line.',
    'LM_EXTENDED: Die Schenkelgerade wird in bis zu drei überlappenden Abschnitten bestimmt, die übereinstimmen müssen. LM_STANDARD: eine Gerade.'],
  LE_POINTS_FIXL3: ['Number of points in the last leg section. Default 60.', 'Punkte im letzten Schenkelabschnitt. Standard 60.'],
  LE_POINTS_MIN: ['Minimum number of leg points for the sections. Default 50.', 'Mindestzahl an Schenkelpunkten für die Abschnitte. Standard 50.'],
  LE_COVER: ['How far the sections may overlap, in percent. Default 99.', 'Wie weit sich die Abschnitte überlappen dürfen, in Prozent. Standard 99.'],
  LE_TOLERANCE: ['Largest allowed difference between the section angles, in degrees. Default 0.4.',
    'Größte erlaubte Abweichung der Abschnittswinkel in Grad. Standard 0,4.'],
};

const $ = (id) => document.getElementById(id);
let lang = 'en';
let status = null;
let activeTab = 'io';
let dies = [];
let selectedDie = -1;
let pollCount = 0;

const t = (text) => (lang === 'de' && DE[text] !== undefined ? DE[text] : text);
const tValue = (value) => (lang === 'de' && DE_VALUES[value] !== undefined ? DE_VALUES[value] : value);
const tip = (key) => TIPS[key][lang === 'de' ? 1 : 0];

function escapeHtml(text) {
  return String(text).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
function formatValue(value, digits = 3) {
  if (typeof value === 'number') return Number.isInteger(value) ? String(value) : value.toFixed(digits);
  return value === null ? '-' : String(value);
}
function yesNo(flag) { return t(flag ? 'yes' : 'no'); }
function hexByte(value) { return '0x' + Number(value).toString(16).padStart(2, '0'); }
function fillGrid(el, rows) {
  el.innerHTML = rows.map(([label, value]) => `<dt>${escapeHtml(t(label))}</dt><dd>${escapeHtml(value)}</dd>`).join('');
}
function clearCanvas(canvas) {
  const ctx = canvas.getContext('2d'), width = canvas.width, height = canvas.height;
  ctx.clearRect(0, 0, width, height);
  return { ctx, width, height };
}
function activate(button, pane, group) {
  document.querySelectorAll(group).forEach((el) => el.classList.remove('active'));
  button.classList.add('active');
  pane.classList.add('active');
}

function setLang(newLang) {
  lang = newLang;
  try { localStorage.setItem('lang', newLang); } catch (e) { /* Storage blocked: the choice lasts for this page only */ }
  document.documentElement.lang = newLang;
  document.querySelectorAll('[data-t]').forEach((el) => { el.textContent = t(el.dataset.t); });
  document.querySelectorAll('[data-tip]').forEach((el) => { el.dataset.text = tip(el.dataset.tip); });
  document.querySelectorAll('[data-tip-title]').forEach((el) => { el.title = tip(el.dataset.tipTitle); });
  document.querySelectorAll('[data-lang]').forEach((button) => button.classList.toggle('active', button.dataset.lang === newLang));
  $('dnc').dataset.empty = t('no DNC message received');
  renderIoTables();
  if (dies.length) renderDies();
  if (activeTab === 'setup') loadSetup();
}

function authHeaders() {
  const password = $('password').value;
  return password ? { Authorization: 'Basic ' + btoa(SERVICE_USER + ':' + password) } : {};
}

function showResult(ok, text) {
  const box = $('toasts'), el = document.createElement('div');
  el.className = 'toast ' + (ok ? 'ok' : 'bad');
  el.textContent = text;
  const close = () => { el.classList.add('out'); setTimeout(() => el.remove(), TOAST_FADE_MS); };
  el.addEventListener('click', close);
  box.appendChild(el);
  while (box.children.length > TOAST_MAX) box.firstChild.remove();
  setTimeout(close, ok ? TOAST_OK_MS : TOAST_ERROR_MS);
}

async function post(path, params) {
  try {
    const response = await fetch(path, {
      method: 'POST',
      headers: Object.assign({ 'Content-Type': 'application/x-www-form-urlencoded' }, authHeaders()),
      body: new URLSearchParams(params).toString(),
    });
    const result = await response.json();
    $('password').classList.toggle('bad', response.status === 401);
    showResult(result.ok, t(result.text));
  } catch (e) {
    showResult(false, t('request failed') + ': ' + e);
  }
}

async function getJson(path) {
  const response = await fetch(path, { cache: 'no-store' });
  return response.json();
}

const sendCommand = (params) => post('/api/cmd', { ...params, enable: $('enable').checked ? 1 : 0 });

function ioRows(rows, lampId, prefix, extraCells) {
  return rows.map((row, i) =>
    `<tr${row[0] === '-' ? ' class="spare"' : ''}><td><span class="lamp" id="${lampId}${i + 1}"></span></td><td>${prefix}${i + 1}</td>` +
    `<td>${escapeHtml(lang === 'de' ? row[1] : row[0])}</td><td class="label">${escapeHtml(row[2])}</td>${extraCells(i + 1)}</tr>`).join('');
}

function renderIoTables() {
  $('inputs').tBodies[0].innerHTML = ioRows(INPUTS, 'in', 'E', () => '');
  $('outputs').tBodies[0].innerHTML = ioRows(OUTPUTS, 'out', 'A', (n) =>
    `<td class="ops"><button data-out="${n}" data-op="on">${t('on')}</button><button data-out="${n}" data-op="off">${t('off')}</button></td>`);
  document.querySelectorAll('[data-out]').forEach((button) => button.addEventListener('click', () => {
    sendCommand({ op: button.dataset.op, n: button.dataset.out });
  }));
  showSwitching();
  if (status) renderIo(status);
}

function showSwitching() {
  const on = $('enable').checked;
  ['outcard', 'unit0', 'unit1'].forEach((id) => $(id).classList.toggle('armed', on));
  $('armedline').classList.toggle('hidden', !on);
  document.querySelectorAll('[data-out], [data-reset], [data-drive], #target0, #target1').forEach((el) => { el.disabled = !on; });
}

function enableChanged() {
  if ($('enable').checked && !$('password').value) {
    $('enable').checked = false;
    $('password').classList.add('bad');
    $('password').focus();
    showResult(false, t('enter the service password first'));
  }
  showSwitching();
}

function buildUnits() {
  for (const unit of UNITS) {
    $('unit' + unit).insertAdjacentHTML('beforeend',
      `<dl class="grid" id="unitdata${unit}"></dl><p class="unitcmd"><button data-reset="${unit}" data-t data-tip-title="reset">reset counter</button>` +
      `<span data-t>drive to</span><input class="pos" id="target${unit}" value="0"><button data-drive="${unit}" data-t data-tip-title="drive">start</button></p>`);
  }
  document.querySelectorAll('[data-reset]').forEach((button) => button.addEventListener('click', () =>
    sendCommand({ op: 'reset', unit: button.dataset.reset })));
  document.querySelectorAll('[data-drive]').forEach((button) => button.addEventListener('click', () =>
    sendCommand({ op: 'drive', unit: button.dataset.drive, target: $('target' + button.dataset.drive).value })));
}

function renderIo(s) {
  for (let n = 1; n <= IO_CHANNELS; n++) {
    $('in' + n).classList.toggle('on', (s.io.in >> (n - 1)) & 1);
    $('out' + n).classList.toggle('on', (s.io.out >> (n - 1)) & 1);
  }
  for (const unit of UNITS) {
    fillGrid($('unitdata' + unit), [['position', s.io.pos[unit]], ['raw counter', s.io.raw[unit]],
      ['drive', tValue(s.io.drive[unit])], ['target', s.io.target[unit]]]);
  }
}

function renderLink(s) {
  const link = s.link;
  fillGrid($('linkdata'), [
    ['device', t(link.open ? 'open' : 'not open')], ['CNC contact', yesNo(link.contact)],
    ['modem lines', link.modem ? t('yes') : t('not available')], ['line supervision', tValue(link.supervision)],
    ['last byte received', hexByte(link.last_rx)], ['last byte sent', hexByte(link.last_tx)],
    ['last command / group', hexByte(link.last_cmd) + ' / ' + link.group], ['parser', tValue(link.parser)],
    ['parameter block / byte', link.block + ' / ' + link.pos], ['bytes received', link.rx_count],
    ['phase p', link.phase], ['set angle', formatValue(link.set_angle, 1)], ['station angle sent', formatValue(link.angle)],
    ['raw value', link.raw + ' (' + hexByte(link.raw) + ')'], ['p20 / p28 / p30', `${link.p20} / ${link.p28} / ${link.p30}`],
    ['referenced / encoder used / leg check', `${yesNo(link.referenced)} / ${yesNo(link.encoder_used)} / ${yesNo(link.leg_check)}`],
    ['DNC messages', link.dnc_count], ['errors (85/81 replies)', link.errors], ['springback queries', link.springback],
  ]);
  $('dnc').textContent = link.dnc_text;
}

function setupText(setup) {
  return `${setup.move_method}, ${t('move down at start')} ${yesNo(setup.move_down_start)}, ${t('secure angle')} ${yesNo(setup.secure_angle)}`;
}

function renderCtl(s) {
  const ctl = s.ctl;
  fillGrid($('ctldata'), [
    ['program version', s.version + (s.sim ? ` (${t('simulation')})` : '')], ['uptime', formatValue(s.uptime, 1) + ' s'],
    ['error', ctl.error + ' – ' + tValue(ctl.error_text)], ['controller cycle', ctl.ctl_cycle_us + ' µs'],
    ['I/O cycle (last / max)', ctl.io_cycle_us + ' / ' + ctl.io_cycle_max_us + ' µs'],
    ['controller state', tValue(ctl.state)], ['reference sequence', tValue(ctl.seq_reference)],
    ['cylinder sequence', tValue(ctl.seq_cylinder)], ['die request sequence', tValue(ctl.seq_die)],
    ['cylinder up', yesNo(ctl.cylinder_up)], ['referenced', yesNo(ctl.referenced)], ['scanning', yesNo(ctl.scanning)],
    ['encoder used', yesNo(ctl.encoder_used)], ['leg check', yesNo(ctl.leg_check)],
    ['control-angle / measure-type / measure-this-bend', `${ctl.control_angle} / ${ctl.measure_type} / ${ctl.measure_this_bend}`],
    ['dies in the controller', ctl.dies],
    ['active setup', `${setupText(ctl.setup)}, ${t('use turn')} ${yesNo(ctl.setup.use_turn)}`],
  ]);
}

function resultRows(res) {
  return [['leg angle α', formatValue(res.leg_angle)], ['die angle δ', formatValue(res.die_angle)],
    ['reference point R', `${formatValue(res.ref_x, 1)}, ${formatValue(res.ref_y, 1)}`],
    ['reference done', yesNo(res.done)], ['candidate points', res.candidates], ['good reference frames g', res.good_refs],
    ['die fits', res.die_fits], ['die points', res.die_points], ['leg points n_P', res.leg_points], ['filtered L', res.filt_last],
    ['reference frame', yesNo(res.ref_frame)],
    ['sections 1–3', `${formatValue(res.section1)} / ${formatValue(res.section2)} / ${formatValue(res.section3)}`],
    ['full line', formatValue(res.full_line)], ['leg line λ', formatValue(res.line_angle)], ['valid', yesNo(res.valid)]];
}

function renderSensors(s) {
  s.sensors.forEach((sensor, unit) => fillGrid($('sens' + unit), [['address', sensor.addr], ['connected', yesNo(sensor.connected)],
    ['frames (valid / invalid)', `${sensor.frames} (${sensor.valid_frames} / ${sensor.invalid_frames})`], ['link phase', sensor.phase]]
    .concat(resultRows(sensor.res))));
  const st = s.station;
  fillGrid($('station'), [['B (both valid once)', yesNo(st.both)], ['valid rear / front', `${yesNo(st.valid_rear)} / ${yesNo(st.valid_front)}`],
    ['rear / front angle', `${formatValue(st.rear)} / ${formatValue(st.front)}`], ['combined angle', formatValue(st.combined)],
    ['held angle', formatValue(st.held)], ['pairs', st.pairs]]);
  drawBend(st);
}

const LEG_LENGTH = 150;
const LEG_ANGLE_MAX = 95; // A leg is drawn at 0 to 95 degrees from the vertical
const DIE_HALF_WIDTH = 150;
const V_HALF_WIDTH = 44; // Half the V opening of the die, also its depth
const PUNCH_HALF_WIDTH = 16;
const radians = (degrees) => degrees * Math.PI / 180;

function drawBend(st) {
  const { ctx, width, height } = clearCanvas($('bend'));
  const centerX = width / 2, dieTop = height - 74, bendX = centerX, bendY = dieTop + 10, dieBottom = height - 6;
  ctx.fillStyle = COLOR.die;
  ctx.beginPath(); ctx.moveTo(centerX - DIE_HALF_WIDTH, dieBottom); ctx.lineTo(centerX - DIE_HALF_WIDTH, dieTop);
  ctx.lineTo(centerX - V_HALF_WIDTH, dieTop); ctx.lineTo(centerX, dieTop + V_HALF_WIDTH); ctx.lineTo(centerX + V_HALF_WIDTH, dieTop);
  ctx.lineTo(centerX + DIE_HALF_WIDTH, dieTop); ctx.lineTo(centerX + DIE_HALF_WIDTH, dieBottom); ctx.closePath(); ctx.fill();
  ctx.fillStyle = COLOR.punch;
  ctx.beginPath(); ctx.moveTo(centerX - PUNCH_HALF_WIDTH, 0); ctx.lineTo(centerX + PUNCH_HALF_WIDTH, 0);
  ctx.lineTo(centerX + PUNCH_HALF_WIDTH, bendY - 60); ctx.lineTo(centerX, bendY - 4);
  ctx.lineTo(centerX - PUNCH_HALF_WIDTH, bendY - 60); ctx.closePath(); ctx.fill();
  const clamp = (angle) => Math.max(0, Math.min(LEG_ANGLE_MAX, angle));
  const half = clamp(st.combined / 2);
  const legs = [
    { angle: st.valid_rear ? clamp(st.rear) : half, valid: st.valid_rear, side: -1, name: 'rear', value: st.rear },
    { angle: st.valid_front ? clamp(st.front) : half, valid: st.valid_front, side: 1, name: 'front', value: st.front },
  ];
  ctx.lineCap = 'round';
  const ends = legs.map((leg) => {
    const endX = bendX + leg.side * LEG_LENGTH * Math.sin(radians(leg.angle));
    const endY = bendY - LEG_LENGTH * Math.cos(radians(leg.angle));
    ctx.setLineDash(leg.valid ? [] : [8, 7]);
    ctx.strokeStyle = leg.valid ? COLOR.ink : COLOR.faint; ctx.lineWidth = 6;
    ctx.beginPath(); ctx.moveTo(bendX, bendY); ctx.lineTo(endX, endY); ctx.stroke();
    return { ...leg, direction: Math.atan2(endY - bendY, endX - bendX) };
  });
  ctx.setLineDash([]);
  ctx.strokeStyle = COLOR.green; ctx.lineWidth = 2; // Angle arc between the legs
  ctx.beginPath(); ctx.arc(bendX, bendY, 44, ends[0].direction, ends[1].direction); ctx.stroke();
  ctx.textAlign = 'center'; ctx.fillStyle = COLOR.green; ctx.font = font(22, true);
  ctx.fillText(`${formatValue(st.combined, 1)}°`, bendX, bendY - 58);
  ctx.font = font(12); ctx.fillStyle = COLOR.muted;
  ctx.fillText(t('bend angle') + (st.valid_rear || st.valid_front ? '' : ` (${t('held')})`), bendX, 18);
  for (const leg of ends) { // Leg labels in the upper corners
    ctx.textAlign = leg.side < 0 ? 'left' : 'right';
    const x = leg.side < 0 ? 10 : width - 10, y = 22;
    ctx.fillStyle = COLOR.ink; ctx.font = font(13, true); ctx.fillText(t(leg.name), x, y);
    ctx.font = font(12); ctx.fillStyle = leg.valid ? COLOR.ink : COLOR.faint;
    ctx.fillText(leg.valid ? `α ${formatValue(leg.value, 2)}°` : t('not measured'), x, y + 16);
  }
  ctx.textAlign = 'start';
}

const NO_LASER_Y = 4095; // y of a record without a laser point
const GOOD_QUALITY = 20; // Points of a higher quality are drawn dark
const PROFILE_MARGIN = 150;
const PROFILE_EMPTY_VIEW = { x0: 500, x1: 2700, y0: 0, y1: 4100 };
const HEADER_HEIGHT = 26; // The text line at the top of a profile
const RAY_LENGTH = 900;
const LABEL_DISTANCE = 110;

function drawProfile(canvas, profile) {
  const { ctx, width, height } = clearCanvas(canvas);
  const res = profile.res;
  const points = profile.points.filter(([, y]) => y < NO_LASER_Y);
  // Scale to the points and the reference point with a margin, same scale on both axes
  const xs = points.map(([x]) => x).concat(res.ref_x ? [res.ref_x] : []);
  const ys = points.map(([, y]) => y).concat(res.ref_x ? [res.ref_y] : []);
  let { x0, x1, y0, y1 } = PROFILE_EMPTY_VIEW;
  if (xs.length) {
    [x0, x1] = [Math.min(...xs) - PROFILE_MARGIN, Math.max(...xs) + PROFILE_MARGIN];
    [y0, y1] = [Math.min(...ys) - PROFILE_MARGIN, Math.max(...ys) + PROFILE_MARGIN];
  }
  const scale = Math.min((width - 20) / (x1 - x0), (height - 36) / (y1 - y0));
  const offsetX = (width - (x1 - x0) * scale) / 2, offsetY = (height - HEADER_HEIGHT - (y1 - y0) * scale) / 2;
  const X = (x) => offsetX + (x - x0) * scale, Y = (y) => height - offsetY - (y - y0) * scale;
  ctx.save();
  ctx.beginPath(); ctx.rect(0, HEADER_HEIGHT, width, height - HEADER_HEIGHT); ctx.clip(); // Keep the header line free
  for (const [x, y, quality] of points) {
    ctx.fillStyle = quality > GOOD_QUALITY ? COLOR.ink : COLOR.weakPoint;
    ctx.fillRect(X(x) - 2, Y(y) - 2, 4, 4);
  }
  const ray = (degrees, color) => {
    const dx = RAY_LENGTH * Math.cos(radians(degrees)), dy = RAY_LENGTH * Math.sin(radians(degrees));
    ctx.strokeStyle = color; ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(X(res.ref_x - dx), Y(res.ref_y - dy)); ctx.lineTo(X(res.ref_x + dx), Y(res.ref_y + dy)); ctx.stroke();
  };
  // Text with a light background, so it stays readable on top of points and lines
  const label = (x, y, text, color) => {
    ctx.font = font(13, true);
    const textWidth = ctx.measureText(text).width;
    x = Math.max(6, Math.min(x, width - textWidth - 6)); y = Math.max(44, Math.min(y, height - 8)); // Stay inside the plot
    ctx.fillStyle = COLOR.labelBackground; ctx.fillRect(x - 3, y - 13, textWidth + 6, 18);
    ctx.fillStyle = color; ctx.fillText(text, x, y);
  };
  const along = (degrees, distance) =>
    [X(res.ref_x) + distance * Math.cos(-radians(degrees)), Y(res.ref_y) + distance * Math.sin(-radians(degrees))];
  if (res.ref_x) {
    ray(res.die_angle, COLOR.orange);
    const leg = res.valid && !res.ref_frame; // Only the leg line of this frame, an old one would mislead
    if (leg) ray(res.line_angle, COLOR.green);
    label(...along(res.die_angle, LABEL_DISTANCE), t('die flank'), COLOR.darkOrange);
    if (leg) label(...along(res.line_angle + 180, LABEL_DISTANCE), t('leg line'), COLOR.green);
    if (leg) { // Leg angle α between die flank and leg line
      const dieAngle = -radians(res.die_angle), lineAngle = -radians(res.line_angle);
      ctx.strokeStyle = COLOR.green; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(X(res.ref_x), Y(res.ref_y), 46, Math.min(dieAngle, lineAngle), Math.max(dieAngle, lineAngle)); ctx.stroke();
      const middle = (dieAngle + lineAngle) / 2;
      ctx.fillStyle = COLOR.green; ctx.font = font(14, true);
      ctx.fillText(`α ${formatValue(res.leg_angle, 2)}°`, X(res.ref_x) + 58 * Math.cos(middle), Y(res.ref_y) + 58 * Math.sin(middle));
    }
    ctx.fillStyle = COLOR.red;
    ctx.beginPath(); ctx.arc(X(res.ref_x), Y(res.ref_y), 5, 0, 2 * Math.PI); ctx.fill();
    label(X(res.ref_x) - 22, Y(res.ref_y) + 4, 'R', COLOR.red);
  }
  ctx.restore();
  ctx.fillStyle = COLOR.muted;
  ctx.font = font(13);
  ctx.fillText(`α ${formatValue(res.leg_angle)}   δ ${formatValue(res.die_angle)}   λ ${formatValue(res.line_angle)}` +
    (points.length ? '' : '   ' + t('no laser points in this frame')), 10, 18);
}

function renderDies() {
  $('dietable').tBodies[0].innerHTML = dies.map((die, i) =>
    `<tr class="${i === selectedDie ? 'sel' : ''}" data-row="${i}"><td><input type="radio" name="die" ${i === selectedDie ? 'checked' : ''}></td>` +
    `<td><input data-f="name" maxlength="11" value="${escapeHtml(die.name)}"></td><td class="num"><input class="pos" data-f="rear" value="${die.rear}"></td>` +
    `<td class="num"><input class="pos" data-f="front" value="${die.front}"></td></tr>`).join('');
  document.querySelectorAll('#dietable tbody tr').forEach((row) => {
    const i = Number(row.dataset.row);
    row.addEventListener('click', () => { if (selectedDie !== i) { selectedDie = i; renderDies(); } });
    row.querySelectorAll('input[data-f]').forEach((input) => input.addEventListener('change', () => {
      dies[i][input.dataset.f] = input.dataset.f === 'name' ? input.value : Number(input.value);
    }));
  });
}
function dieText() { return dies.map((die) => `${die.name},${die.rear},${die.front}`).join('\n'); }
async function loadDies() { dies = (await getJson('/api/dies')).dies; selectedDie = -1; renderDies(); }

const HISTORY = { time: 0, rear: 1, front: 2, combined: 3, phase: 4 }; // Columns of an /api/history entry
const HISTORY_ANGLE_MAX = 200;
const HISTORY_GRID_STEP = 20;
const PHASE_SCALE = 10; // The phase is drawn × 10

function drawHistory(entries) {
  const { ctx, width, height } = clearCanvas($('hist'));
  if (!entries.length) return;
  const startS = entries[0][HISTORY.time], endS = Math.max(entries[entries.length - 1][HISTORY.time], startS + 1);
  const X = (s) => 40 + (s - startS) / (endS - startS) * (width - 50);
  const Y = (angle) => height - 20 - angle / HISTORY_ANGLE_MAX * (height - 30);
  ctx.strokeStyle = COLOR.grid; ctx.fillStyle = COLOR.muted;
  for (let angle = 0; angle <= HISTORY_ANGLE_MAX; angle += HISTORY_GRID_STEP) {
    ctx.beginPath(); ctx.moveTo(40, Y(angle)); ctx.lineTo(width - 10, Y(angle)); ctx.stroke(); ctx.fillText(angle + '°', 4, Y(angle) + 4);
  }
  const series = (column, color, scale = 1) => {
    ctx.strokeStyle = color; ctx.beginPath();
    entries.forEach((entry, i) => {
      const x = X(entry[HISTORY.time]), y = Y((entry[column] ?? 0) * scale);
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    });
    ctx.stroke();
  };
  series(HISTORY.rear, COLOR.blue); series(HISTORY.front, COLOR.orange); series(HISTORY.combined, COLOR.green);
  series(HISTORY.phase, COLOR.purple, PHASE_SCALE);
  ctx.fillText(`${t('last')} ${formatValue(endS - startS, 1)} s`, width - 90, 14);
}

async function loadSetup() {
  const setup = await getJson('/api/setup');
  const form = $('setupform');
  for (const [key, value] of Object.entries(setup.file)) {
    const el = form.elements[key];
    if (!el) continue;
    if (el.type === 'checkbox') el.checked = value; else el.value = value;
  }
  $('activesetup').textContent = `${t('active')}: ${setupText(setup.active)}`;
}

function showConnection(online) {
  const conn = $('conn'), text = online ? 'online' : 'offline';
  conn.dataset.t = text; conn.textContent = t(text); conn.className = 'tag ' + (online ? 'ok' : 'bad');
}

async function poll() {
  try {
    status = await getJson('/api/status');
    showConnection(true);
    $('version').textContent = status.version;
    $('sim').classList.toggle('hidden', !status.sim);
    const ctl = status.ctl, link = status.link;
    $('summary').textContent = `${tValue(ctl.state)} · ${t('error')} ${ctl.error} · ${t('cylinder')} ${t(ctl.cylinder_up ? 'up' : 'down')} · ` +
      `${t(ctl.referenced ? 'referenced' : 'not referenced')} · ${t('phase')} ${link.phase} · ${t('angle')} ${formatValue(status.station.combined, 2)}`;
    renderIo(status);
    if (activeTab === 'link') renderLink(status);
    if (activeTab === 'ctl') renderCtl(status);
    if (activeTab === 'sensors') {
      renderSensors(status);
      const unit = pollCount % UNITS.length; // One profile per poll, the sensors take turns
      drawProfile($('prof' + unit), await getJson('/api/profile?s=' + unit));
    }
    const slowPoll = pollCount % SLOW_POLL_EVERY === 0;
    if (slowPoll && activeTab === 'history') drawHistory((await getJson('/api/history')).entries);
    if (slowPoll && activeTab === 'log') {
      const pre = $('logtext'), atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 4;
      pre.textContent = (await getJson('/api/log')).lines.join('\n');
      if (atEnd) pre.scrollTop = pre.scrollHeight;
    }
  } catch (e) {
    showConnection(false);
  }
  pollCount++;
  setTimeout(poll, POLL_MS);
}

function init() {
  buildUnits();
  document.querySelectorAll('[data-t]').forEach((el) => { el.dataset.t = el.textContent.trim(); });
  let savedLang = null;
  try { savedLang = localStorage.getItem('lang'); } catch (e) { /* Storage blocked: use the browser language */ }
  document.querySelectorAll('[data-lang]').forEach((button) => button.addEventListener('click', () => setLang(button.dataset.lang)));
  $('password').value = sessionStorage.getItem('pw') || '';
  $('password').addEventListener('change', () => sessionStorage.setItem('pw', $('password').value));
  $('password').addEventListener('input', () => $('password').classList.remove('bad'));
  $('enable').addEventListener('change', enableChanged);
  document.querySelectorAll('#tabs button').forEach((button) => button.addEventListener('click', () => {
    activate(button, $(button.dataset.tab), '#tabs button, .tab');
    activeTab = button.dataset.tab;
    if (activeTab === 'dies') loadDies();
    if (activeTab === 'setup') loadSetup();
  }));
  document.querySelectorAll('.subtabs button').forEach((button) => button.addEventListener('click', () => {
    activate(button, $('sub-' + button.dataset.sub), '.subtabs button, .sub');
  }));
  $('die_add').onclick = () => { dies.push({ name: 'NEW', rear: 0, front: 0 }); selectedDie = dies.length - 1; renderDies(); };
  $('die_del').onclick = () => { if (selectedDie >= 0) { dies.splice(selectedDie, 1); selectedDie = -1; renderDies(); } };
  $('die_take').onclick = () => {
    if (selectedDie < 0 || !status) return showResult(false, t('select a row first'));
    dies[selectedDie].rear = status.io.pos[0]; dies[selectedDie].front = status.io.pos[1]; renderDies();
  };
  $('die_save').onclick = () => post('/api/dies', { action: 'save', dies: dieText() });
  $('die_send').onclick = () => post('/api/dies', { action: 'send', dies: dieText() });
  $('die_list').onclick = () => post('/api/dies', { action: 'list' });
  $('setupform').addEventListener('submit', async (event) => {
    event.preventDefault();
    const params = {};
    for (const el of event.target.elements) {
      if (!el.name) continue;
      params[el.name] = el.type === 'checkbox' ? (el.checked ? 'TRUE' : 'FALSE') : el.value;
    }
    await post('/api/setup', params);
    loadSetup();
  });
  $('setup_cancel').onclick = loadSetup;
  setLang(savedLang || ((navigator.language || '').toLowerCase().startsWith('de') ? 'de' : 'en'));
  poll();
}

init();
