# SprinkleScriptMaker

**SprinkleScriptMaker** is a lightweight, modern C++ desktop application designed to easily create, edit, and export script files (`.txt`) for the **Sprinkle Tool** in Valve's Hammer and Hammer++ Editors.

Writing Sprinkle configuration files manually can be error-prone and tedious. This tool streamlines the level design workflow by providing an intuitive user interface to configure prop categories, base rules, and model probabilities.

---

## 🛠️ Built With

* **C++20** - Modern C++ core logic
* **[SourcePP](https://github.com/craftablescience/sourcepp)** - Native Valve file format parsing (KeyValues/VDF, VPK, MDL)
* **CMake** - Cross-platform build automation

---

## ✨ Planned Features

- [x] Modern KeyValues `.txt` script generation
- [ ] User-friendly GUI for defining presets, base parameters, and probabilities
- [ ] Direct VPK browsing for Source Engine game assets
- [ ] 3D Model & Material preview

---

## 🚀 Building from Source

### Prerequisites
* **C++20 Compiler** (MSVC 2022, GCC 10+, or Clang 11+)
* **CMake** (v3.15 or higher)
* **Git**

### Build Steps

1. Clone the repository:
   ```bash
   git clone [https://github.com/YOUR_USERNAME/SprinkleScriptMaker.git](https://github.com/YOUR_USERNAME/SprinkleScriptMaker.git)
   cd SprinkleScriptMaker
