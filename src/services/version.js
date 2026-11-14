/* 应用版本号 —— 唯一来源。
 *
 * 改这里必须同时改 package.json 的 "version"，然后重新构建；
 * 发布 release 时 tag 也要用同一个号。三处（本文件 / package.json / git tag）
 * 必须一致，否则会出现「release 是 v1.0.3、软件里写 1.0.0」这种不自洽
 * （用户 m14526 报的就是这个）。
 *
 * 注意：src/services/net.js 里的 `X-Ver: 1.0.0` 是**冒充官方 web 客户端**的
 * 请求头，不是我们的版本号，不要跟着改。 */
export const APP_VERSION = '1.2.3';
