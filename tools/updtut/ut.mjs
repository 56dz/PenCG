// 主机侧单测：直接 import 真实的 src/services/updater.js（不是复刻模型）。
// 依赖的 net.js / log.js / storage.js / version.js 由 stub/ 下的空壳替换，
// 这样纯逻辑部分可以脱离设备与 QuickJS 跑。
//
// 跑法：bash PenCG/tools/updtut/run.sh
import * as U from './updater.link.js';
let fail = 0, n = 0;
function eq(got, want, tag) {
  n++;
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) { fail++; console.log('FAIL', tag, '| got', a, '| want', b); }
}
// --- parseVersion: {nums, pre} | null
eq(U.parseVersion('v1.2.3').nums, [1,2,3], 'parseVersion v-prefix');
eq(U.parseVersion('v1.2.3').pre, '', 'parseVersion no pre');
eq(U.parseVersion('1.2').nums, [1,2], 'parseVersion short keeps raw length');
eq(U.parseVersion(''), null, 'parseVersion empty -> null');
eq(U.parseVersion(null), null, 'parseVersion non-string -> null');
eq(U.parseVersion('abc'), null, 'parseVersion garbage -> null');
eq(U.parseVersion('v1.2.3-beta.1'), {nums:[1,2,3], pre:'beta.1'}, 'parseVersion pre');
// --- compareVersion
eq(U.compareVersion('1.2.3','1.2.4'), -1, 'cmp less');
eq(U.compareVersion('1.2.4','1.2.3'), 1, 'cmp greater');
eq(U.compareVersion('1.10.0','1.9.9'), 1, 'cmp numeric not lexical');
eq(U.compareVersion('1.3','1.3.0'), 0, 'cmp padding zeros');
eq(U.compareVersion('1.0.0-beta','1.0.0'), -1, 'cmp pre is less');
eq(U.compareVersion('bad','1.0.0'), 0, 'cmp unparsable -> 0 (never fake-update)');
// --- isNewer
eq(U.isNewer('v1.2.4','1.2.3'), true, 'newer yes');
eq(U.isNewer('v1.2.3','1.2.3'), false, 'newer same');
eq(U.isNewer('v1.2.2','1.2.3'), false, 'newer older');
eq(U.isNewer('junk','1.2.3'), false, 'newer unparsable');
// --- normalizeRelease: flat shape
const base = { tag_name:'v9.9.9', body:'x', assets:[{name:'PenCG.amr', browser_download_url:'https://a/b/PenCG.amr', size: 123}] };
eq(U.normalizeRelease({...base, draft:true}, 'r'), null, 'draft filtered');
eq(U.normalizeRelease({...base, prerelease:true}, 'r'), null, 'prerelease filtered');
eq(U.normalizeRelease({...base, release_status:'pre'}, 'r'), null, 'release_status pre filtered');
const nr = U.normalizeRelease(base, 'https://gitcode.com/dz56dz/PenCG');
eq(nr.tag, 'v9.9.9', 'tag');
eq(nr.version, '9.9.9', 'version without v');
eq(nr.assetName, 'PenCG.amr', 'asset picked by ASSET_NAME');
eq(nr.assetSize, 123, 'asset size');
eq(nr.assetUrl, 'https://a/b/PenCG.amr', 'asset url');
const nr2 = U.normalizeRelease({...base, assets:[{name:'other.zip', browser_download_url:'https://a/o.zip', size:9}]}, 'r');
eq(nr2.assetName, 'other.zip', 'asset fallback to assets[0]');
// 关键：当 PenCG.amr 存在但**不是第一个**时必须仍然选中它。
// （发布时源码包 v1.2.3.zip / .tar.gz 会排在资产列表最前面，若不按名字挑，
//   用户会下到一份源码压缩包 —— 这正是 v1.2.3 首发时踩过的坑。）
const nrNamed = U.normalizeRelease({ tag_name:'v9.9.9', assets:[
  {name:'v9.9.9.zip', browser_download_url:'https://a/src.zip', size: 11},
  {name:'v9.9.9.tar.gz', browser_download_url:'https://a/src.tar.gz', size: 12},
  {name:'PenCG.amr', browser_download_url:'https://a/PenCG.amr', size: 333069}
]}, 'r');
eq(nrNamed.assetName, 'PenCG.amr', 'asset picked by name even when not first');
eq(nrNamed.assetUrl, 'https://a/PenCG.amr', 'named asset url');
eq(nrNamed.assetSize, 333069, 'named asset size');
eq(U.normalizeRelease({tag_name:'not-a-version'}, 'r'), null, 'bad tag -> null');
eq(U.normalizeRelease('{"tag_name":"v1.0.0"}', 'r').tag, 'v1.0.0', 'string body parsed');
eq(U.normalizeRelease('{bad json', 'r'), null, 'bad json -> null');
// --- isMirrorable / downloadUrl
eq(U.isMirrorable('https://github.com/x/y/releases/download/v1/a.amr'), true, 'mirrorable github');
eq(U.isMirrorable('https://objects.githubusercontent.com/x/a.amr'), true, 'mirrorable githubusercontent');
eq(U.isMirrorable('https://gitcode.com/x/y/a.amr'), false, 'gitcode not mirrored');
eq(U.isMirrorable('https://evilgithub.com/a.amr'), false, 'evilgithub rejected');
eq(U.isMirrorable('https://github.com.evil.io/a.amr'), false, 'github.com.evil rejected');
eq(U.isMirrorable('ftp://github.com/a'), false, 'non-http rejected');
eq(U.downloadUrl('https://github.com/x/y/a.amr', false), 'https://github.com/x/y/a.amr', 'no mirror passthrough');
eq(U.downloadUrl('https://github.com/x/y/a.amr', true), U.MIRROR_PREFIX + 'https://github.com/x/y/a.amr', 'mirror prefix');
eq(U.downloadUrl('https://gitcode.com/x/y/a.amr', true), 'https://gitcode.com/x/y/a.amr', 'gitcode never mirrored');
// --- bodyLines: array
const bl = U.bodyLines('v1.2.3\n\n## 修复\n- a\n- b\n\n\n## 其他\ntext');
eq(Array.isArray(bl), true, 'bodyLines array');
eq(bl.includes('v1.2.3'), false, 'leading bare version dropped');
eq(bl.includes('修复'), true, 'heading hashes stripped');
eq(bl.includes('· a'), true, 'list marker becomes dot');
eq(bl.some(l => l === '其他'), true, 'second heading kept');
eq(U.bodyLines('a\nb\nc', 2).length, 2, 'maxLines cap');
// 连续空行必须压成一个，且首尾不留空
eq(U.bodyLines('a\n\n\n\n\nb'), ['a','','b'], 'consecutive blanks collapsed');
eq(U.bodyLines('\n\n\na\n\n'), ['a'], 'leading/trailing blanks trimmed');
eq(U.bodyLines(null).length, 0, 'null body');
eq(U.bodyLines('x').length, 1, 'single line');
eq(U.bodyLines('**bold** `code` [link](u)').join('|'), 'bold code link', 'inline markdown stripped');
// --- dayKey
eq(/^\d{4}-\d{2}-\d{2}$/.test(U.dayKey(Date.now())), true, 'dayKey format');
eq(U.dayKey(0), U.dayKey(0), 'dayKey stable');
// --- normalizeUpdateState
eq(U.normalizeUpdateState(null), { skipVersion:'', quietDay:'' }, 'state null');
eq(U.normalizeUpdateState({skipVersion:'v1'}), { skipVersion:'v1', quietDay:'' }, 'state partial');
eq(U.normalizeUpdateState('junk'), { skipVersion:'', quietDay:'' }, 'state junk');
// --- shouldAutoPrompt(state, remoteTag, localVersion, now)
const now = Date.now();
eq(U.shouldAutoPrompt({skipVersion:'v1.2.4', quietDay:''}, 'v1.2.4', '1.2.3', now), false, 'skipVersion blocks');
eq(U.shouldAutoPrompt({skipVersion:'', quietDay:U.dayKey(now)}, 'v1.2.4', '1.2.3', now), false, 'quietDay blocks');
eq(U.shouldAutoPrompt({skipVersion:'', quietDay:''}, 'v1.2.4', '1.2.3', now), true, 'prompt when newer');
eq(U.shouldAutoPrompt({skipVersion:'', quietDay:''}, 'v1.2.3', '1.2.3', now), false, 'no prompt when same');
eq(U.shouldAutoPrompt({skipVersion:'', quietDay:''}, 'v1.2.2', '1.2.3', now), false, 'no prompt when older');
// --- progressText
eq(typeof U.progressText(0,0), 'string', 'progress string');
eq(U.progressText(50,100).includes('50'), true, 'progress percent shows');
// --- extractFilePath（三种返回形态）
eq(U.extractFilePath({result:'/a/b.amr'}), '/a/b.amr', 'extract string result');
eq(U.extractFilePath({result:{filePath:'/a/c.amr'}}), '/a/c.amr', 'extract result.filePath');
eq(U.extractFilePath({filePath:'/a/d.amr'}), '/a/d.amr', 'extract filePath');
eq(U.extractFilePath({}), '', 'extract none');
// --- sourceById / sourceLabel
eq(U.sourceById('github').id, 'github', 'sourceById hit');
eq(U.sourceById('nope').id, 'gitcode', 'sourceById fallback to primary');
eq(U.sourceLabel('gitcode'), 'GitCode', 'sourceLabel');
eq(U.SOURCES.length, 2, 'two sources');
eq(U.SOURCES[0].id, 'gitcode', 'gitcode is primary (mainland reachability)');
// --- 发布链路硬约束：资产名必须与 ASSET_NAME 一致
eq(U.ASSET_NAME, 'PenCG.amr', 'ASSET_NAME pinned');
eq(U.SAVE_DIR, '/userdisk/miniapp/downloaded', 'SAVE_DIR');
// --- updateCapability
eq(typeof U.updateCapability(), 'object', 'capability object');
console.log(`\n${n - fail}/${n} passed, ${fail} failed`);
if (fail) {
  console.log('提示：若失败项集中在「asset picked by name」上，说明 normalizeRelease');
  console.log('      的资产挑选逻辑被改坏了 —— 那会让用户下到源码压缩包而不是 .amr。');
}
process.exit(fail ? 1 : 0);
