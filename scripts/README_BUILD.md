# BeltWinder Build System

## UI Kompression

Die Web-UI wird automatisch vor jedem Build komprimiert:
- Minifizierung: HTML-Kommentare und Whitespace entfernen
- GZIP Level 9: Maximum Kompression
- Output: `.pio/build/<environment>/index_html_gz.h`

### Änderungen am UI

1. Editiere `main/web_ui.html`
2. Führe `pio run` aus
3. Fertig! Die Kompression läuft automatisch

### Manuelle Kompression (Testing)

