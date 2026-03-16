#!/usr/bin/env python3
"""Generate minimal validation .cpp stubs for each generated .h file.

Each stub simply includes the header and compiles it.  All static_assert
register-index checks are emitted directly by the C++ code generator into
the header itself, so no extra logic is needed here.
"""

from pathlib import Path

if __name__ == '__main__':
    workspace_root = Path(__file__).parent
    header_dir = workspace_root / 'test' / 'output' / 'cpp'

    if not header_dir.exists():
        print(f"Error: {header_dir} not found")
        exit(1)

    generated_files = []

    for header_file in sorted(header_dir.glob('*.h')):
        header_name = header_file.name
        test_file_name = f'validation_{header_name[:-2]}.cpp'
        test_file_path = header_dir / test_file_name

        code = '\n'.join([
            f'// Auto-generated validation stub for {header_name}',
            '// Compile-time layout and register-index validation is done via',
            '// static_assert statements emitted directly into the header.',
            '',
            '#include <windows.h>',
            '#include <DirectXMath.h>',
            '',
            f'#include "{header_name}"',
            '',
            'int main() { return 0; }',
            '',
        ])

        with open(test_file_path, 'w') as f:
            f.write(code)

        generated_files.append(test_file_name)

    print(f"Generated {len(generated_files)} validation files:")
    for fname in generated_files:
        print(f"  - {fname}")
