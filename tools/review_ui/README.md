# CrowPanel Review UI

Local browser tool for reviewing imported CrowPanel screenshots or photos.

## What it does

- Imports a screenshot or photo from disk
- Displays it on a canvas for annotation
- Lets you draw freehand, rectangles, and arrows
- Copies a chat-ready payload you can paste into this conversation
- Optionally saves a local review session folder

## How to launch

```bash
python tools/launch_review_ui.py
```

Then open the shown URL in any modern browser.

## Basic flow

1. Click `Import Screenshot`
2. Choose a saved screenshot or photo
3. Draw annotations and write notes
4. Click `Copy For Chat`
5. Paste the copied block into the conversation

## Notes

- The local Python backend now only serves the UI and saves sessions
- No serial/COM port access is required
- BMP screenshots from the CrowPanel TF card can be imported directly
