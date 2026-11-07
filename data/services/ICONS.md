# Connector icons

Most connector icons are the official favicons or brand marks of the
corresponding service. They are used only to identify the service in the
connector list.

Brand names, logos and icons remain trademarks or copyright of their owners.
They are not covered by the project's MIT license. If you redistribute the
application, check the brand guidelines of each service before reusing its
mark.

Sources for icons added or refreshed during the connector cleanup:

| Connector | Source |
|---|---|
| `google_translate`, `google_new` | <https://translate.google.com/favicon.ico> |
| `yandexinl` | <https://yandex.com/favicon.ico> |
| `sogou` | <https://dlweb.sogoucdn.com/translate/wap/static/img/logo_gray_80.png> |
| `promt` | <https://www.online-translator.com/img/mebius_logo.png> |
| `translateeu` | <https://www.translate.eu/favicon.ico> |
| `lara`, `lara_translate` | <https://laratranslate.com/favicon-32x32.png> |
| `abbyy_lingvo_live` | <https://www.lingvolive.com/images/iosIcons/logo_120.png> |
| `oxford_learner_dictionary` | <https://www.oxfordlearnersdictionaries.com/favicon.ico> |
| `wordreference` | <https://upload.wikimedia.org/wikipedia/commons/b/be/Logo_wordreference.jpg> (cropped) |
| `nuspell` | <https://github.com/nuspell.png> |

`bing_translate`, `bing_tts`, `libretranslate`, `mozhi`, `imtranslator`,
`multitran` and `urban_dictionary` reuse the 64x64 PNG icons already present
in the repository. Their manifests now point to `icon.png` instead of
`icon.svg` or a 16x16 `icon.ico`, so Qt displays the same sharp icon
everywhere.
