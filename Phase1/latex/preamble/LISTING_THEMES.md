# Code Listing Themes Guide

## Overview
The LaTeX template now supports **three professional syntax highlighting themes** for code listings across Python, Assembly, C, and Verilog languages:

1. **Darcula** - Dark, professional PyCharm theme (default)
2. **Dark 2026** - Modern dark theme with vibrant accents
3. **Bright 2026** - Light, clean theme with warm accents

## Key Improvements
✅ Fixed parentheses and bracket coloring issues  
✅ All special characters now properly highlighted  
✅ Complete literate replacement coverage  
✅ Consistent theme across all supported languages  
✅ Easy theme switching commands  

## Supported Languages
- Python (`pythonstyle`)
- Assembly x86 (`asmstyle`)
- C (`cstyle`)
- Verilog (`verilogstyle`)

## Usage Examples

### Default Usage (Darcula Theme)
```latex
\begin{lstlisting}[style=pythonstyle]
def calculate(x, y):
    result = (x + y) * 2
    return result
\end{lstlisting}
```

### Switch to Dark 2026 Theme
```latex
\usePythonDark
\begin{lstlisting}[style=pythonstyle_dark_2026]
def calculate(x, y):
    result = (x + y) * 2
    return result
\end{lstlisting}
```

### Switch to Bright 2026 Theme
```latex
\usePythonBright
\begin{lstlisting}[style=pythonstyle_bright_2026]
def calculate(x, y):
    result = (x + y) * 2
    return result
\end{lstlisting}
```

## Theme-Specific Commands

### Python
- `\usePythonDarcula` - Use Darcula theme
- `\usePythonDark` - Use Dark 2026 theme
- `\usePythonBright` - Use Bright 2026 theme

### Assembly
- `\useAssemblyDarcula`
- `\useAssemblyDark`
- `\useAssemblyBright`

### C
- `\useCDarcula`
- `\useCDark`
- `\useCBright`

### Verilog
- `\useVerilogDarcula`
- `\useVerilogDark`
- `\useVerilogBright`

## Global Theme Switching

Switch all code blocks to the same theme:
```latex
\switchToTheme{dark_2026}
```

Available options: `darcula`, `dark_2026`, `bright_2026`

## Theme Color Characteristics

### Darcula
- Background: Dark gray (#2B2B2B)
- Foreground: Light gray (#A9B7C6)
- Keywords: Orange (#CC7832)
- Strings: Green (#6A8759)
- Numbers: Light blue (#6897BB)
- Brackets: Light gray

### Dark 2026
- Background: Very dark blue (#141820)
- Foreground: Light blue-gray (#C8D2DC)
- Keywords: Bright orange (#FF8C32)
- Strings: Bright green (#78C878)
- Numbers: Bright cyan (#64B4FF)
- Brackets: Bright red-pink (#FF7878)

### Bright 2026
- Background: Off-white (#FAFAFA)
- Foreground: Dark blue-gray (#282832)
- Keywords: Dark orange (#B45014)
- Strings: Dark green (#148232)
- Numbers: Dark blue (#0064C8)
- Brackets: Dark red (#C81E32)

## Direct Style Usage

You can also reference styles directly in the `lstlisting` environment:

```latex
\begin{lstlisting}[style=cstyle_dark_2026]
int main() {
    printf("Hello, World!\n");
    return 0;
}
\end{lstlisting}
```

## Notes
- All themes properly color parentheses, brackets, and operators
- The default master styles use Darcula theme
- Literate replacements handle single and multi-character operators
- Escape sequences in strings are properly highlighted
