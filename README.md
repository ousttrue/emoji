# emoji

Windows build of below code.

https://gist.github.com/jokertarot/7583938

vcvarsall.bat

からvscodeを起動するべし。

## build

* vspkgのfreetypeをリンクした

## usage

Windowsでは、コマンドラインからUTF8文字列を渡すことができなかったのでコードポイントを16進数で指定することにした。

```
> emoji.exe NotoColorEmoji.ttf 1F6000
```
