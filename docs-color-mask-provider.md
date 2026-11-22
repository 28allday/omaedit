# Colour mask provider protocol

> **Not offered in OmaEdit 0.2.0.** Nothing that comes with OmaEdit runs in this
> hook, so the Colour page shows no way to it and the command line lists none of
> its calls (`Backend::kMaskProviderOffered`). The Colour page's built-in AI mask
> of the people in the picture needs no provider. What follows describes the
> protocol for when the hook is offered again.

Colour ▸ Nodes ▸ Face / object mask (AI provider) runs the executable you configure
only when you press **Generate Mask (AI)**. OmaEdit includes the interface, not a
face/object recognition model. A provider can wrap a local model or a service;
its own settings determine model access and network use.

Configure an absolute executable path and an argument template. The default is
`--request {request} --output {output}`. Argument quoting follows
`QProcess::splitCommand`; no shell is involved. Supported substitutions are:

| Placeholder | File |
| --- | --- |
| `{request}` | UTF-8 JSON request |
| `{input}` | Ungraded, isolated clip frame as PNG |
| `{output}` | Destination PNG matte |

The input follows the selected cut's trims and retiming. Its maximum width is
1280 pixels. The request contains:

```json
{
  "protocol": "omaedit.mask.v1",
  "input": "/temporary/request/input.png",
  "output": "/temporary/request/matte.png",
  "kind": "object",
  "prompt": "the person in the foreground",
  "width": 1280,
  "height": 720,
  "frame": 42
}
```

`kind` is `face` or `object`; `frame` is relative to the current timeline cut.
Write a PNG with exactly the input dimensions. Black excludes pixels, white
includes them, and grey gives partial coverage. Exit with status 0 only after
the file is complete. Optional stdout lines can report progress:

```json
{"progress": 45, "message": "Refining edges"}
```

Progress is clamped to 0–99 until the result arrives. Requests time out after two
minutes. **Cancel Request** kills the running provider process and discards the
result. Providers should finish work within that process or forward termination
to their own workers. Output is bounded to 32 megapixels / 128 MiB.

Successful mattes are copied into OmaEdit's application data `ColorMasks/` folder,
then attached to the original correction node as one undoable edit. A result is
rejected if the target clip, node, trim, speed or sequence changed while it ran.
An untouched clip receives its first correction on success. Closing the dialog
allows the request to continue; leaving/loading a project cancels it.

The result is a **still matte** over the clip. Review it in Highlight view,
especially for moving subjects. Connections and Key can remove or replace the
matte and choose luminance or alpha. Generated mattes and LUTs remain external
assets; copy them and relink paths when moving the project to another machine.
