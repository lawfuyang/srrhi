#!/usr/bin/env python3
"""Generate minimal validation .cpp stubs for each generated .h file."""

from pathlib import Path

if __name__ == '__main__':
    workspace_root = Path(__file__).parent
    header_dir = workspace_root / 'test' / 'output' / 'cpp'
    
    if not header_dir.exists():
        print(f"Error: {header_dir} not found")
        exit(1)
    
    # Generate minimal stub for each header file
    generated_files = []
    
    for header_file in sorted(header_dir.glob('*.h')):
        header_name = header_file.name
        test_file_name = f'validation_{header_name[:-2]}.cpp'
        test_file_path = header_dir / test_file_name
        
        code = [
            f'// Auto-generated validation stub for {header_name}',
            '// All validation occurs at compile time via friend validator structs in the header.',
            '',
            '#include <windows.h>',
            '#include <DirectXMath.h>',
            '',
            f'#include "{header_name}"',
            '',
            'int main() {',
            '    return 0;',
            '}',
            ''
        ]
        
        with open(test_file_path, 'w') as f:
            f.write('\n'.join(code))
        
        generated_files.append(test_file_name)
    
    print(f"Generated {len(generated_files)} validation files:")
    for fname in generated_files:
        print(f"  - {fname}")
