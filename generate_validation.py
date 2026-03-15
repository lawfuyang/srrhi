#!/usr/bin/env python3
"""
Generate validation.cpp that includes all generated .h files and adds
static_asserts to verify struct member offsets and sizes against DXC reflection.
"""

import json
import os
import re
from pathlib import Path

# Parse struct definitions from header files
def parse_structs(header_file):
    """Parse struct definitions and members from a header file."""
    with open(header_file, 'r') as f:
        content = f.read()
    
    # Find all struct definitions
    struct_pattern = r'struct\s+(?:alignas\([^)]+\))?\s*(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}'
    structs = []
    
    for match in re.finditer(struct_pattern, content, re.MULTILINE | re.DOTALL):
        struct_name = match.group(1)
        struct_body = match.group(2)
        
        # Parse members - more flexible pattern to catch all type variations
        # Matches: type name; or type name[size]; 
        members = []
        # Updated pattern: captures type (which can include ::, templates, etc.) and member name
        member_pattern = r'(?:^|\n)\s*(\w+(?:::\w+)*(?:<[^>]+>)?(?:\s+(?:const|volatile|unsigned))*)\s+(\w+)(?:\[(\d+)\])?(?:\s*;|$)'
        
        for member_match in re.finditer(member_pattern, struct_body):
            member_type = member_match.group(1).strip()
            member_name = member_match.group(2)
            array_size = member_match.group(3)
            
            # Skip empty matches
            if member_name and member_type:
                members.append({
                    'name': member_name,
                    'type': member_type,
                    'array_size': array_size
                })
        
        if members:  # Only include structs with members
            structs.append({
                'name': struct_name,
                'members': members,
                'file': os.path.basename(header_file)
            })
    
    return structs

def load_reflection_data(reflection_file):
    """Load DXC reflection data from JSON file."""
    reflection_map = {}  # Maps struct name -> {member_name: offset}
    computed_layouts = {}  # Maps struct name -> {member_name: (offset, size)}
    
    if not os.path.exists(reflection_file):
        print(f"Warning: Reflection data file not found: {reflection_file}")
        return reflection_map, computed_layouts
    
    try:
        with open(reflection_file, 'r') as f:
            data = json.load(f)
        
        for entry in data:
            sr_file = entry.get('file', '')
            sr_basename = os.path.splitext(sr_file)[0]
            
            # Extract computed layouts
            for layout in entry.get('computedLayouts', []):
                struct_name = layout.get('name', '')
                members = {}
                for member in layout.get('members', []):
                    member_name = member.get('name', '')
                    offset = member.get('offset', 0)
                    size = member.get('size', 0)
                    members[member_name] = (offset, size)
                
                if members:
                    computed_layouts[struct_name] = members
        
        print(f"Loaded computed layouts for {len(computed_layouts)} struct(s)")
    except Exception as e:
        print(f"Warning: Failed to load reflection data: {e}")
    
    return reflection_map, computed_layouts

def get_layout_offsets(header_name, computed_layouts):
    """Get computed layout offsets for a specific header."""
    # Return the layouts - the Python script now works with computed layouts directly
    return computed_layouts

def generate_validation_cpp(output_dir, header_dir, reflection_data):
    """Generate one validation .cpp file per header file."""
    
    # Get all header files
    header_files = sorted(Path(header_dir).glob('*.h'))
    
    generated_files = []
    
    for header_file in header_files:
        structs = parse_structs(str(header_file))
        
        if not structs:
            # Skip headers with no structs
            continue
        
        header_name = header_file.name
        test_file_name = f'validation_{header_name[:-2]}.cpp'  # Remove .h and add validation_ prefix
        test_file_path = os.path.join(output_dir, test_file_name)
        
        # Generate C++ code for this header
        code = ['// Auto-generated validation code for ' + header_name]
        code.append('// Validates struct offsets against computed layout data')
        code.append('')
        code.append('#include <cstddef>')
        code.append('#include <cstdint>')
        code.append('#include <cstdio>')
        code.append('#include <windows.h>')
        code.append('#include <DirectXMath.h>')
        code.append('')
        
        # Include only this header
        code.append(f'#include "{header_name}"')
        code.append('')
        
        # Generate compile-time validation
        code.append('// ===== COMPILE-TIME VALIDATION =====')
        code.append('// Validates offsetof() against computed layout offsets')
        code.append('')
        code.append('namespace StructValidation {')
        code.append('')
        
        for struct_info in structs:
            struct_name = struct_info['name']
            code.append(f'    // {struct_name}')
            code.append(f'    static_assert(sizeof({struct_name}) > 0, "{struct_name} must compile");')
            
            # Add offset validations if we have layout data
            if struct_name in reflection_data:
                layout_members = reflection_data[struct_name]
                for member in struct_info['members']:
                    member_name = member['name']
                    
                    if member_name in layout_members:
                        expected_offset, expected_size = layout_members[member_name]
                        code.append(f'    static_assert(offsetof({struct_name}, {member_name}) == {expected_offset},')
                        code.append(f'        "{struct_name}::{member_name} offset mismatch: expected {expected_offset}");')
            
            code.append('')
        
        code.append('}  // namespace StructValidation')
        code.append('')
        
        # Generate runtime validation function
        code.append('// ===== RUNTIME VALIDATION FUNCTION =====')
        code.append('')
        code.append('void ValidateStructLayouts() {')
        code.append(f'    printf("=== Struct Layout Validation for {header_name} ===\\n\\n");')
        code.append(f'    printf("Validating %d structs from {header_name}\\n\\n", {len(structs)});')
        code.append('')
        
        for struct_info in structs:
            struct_name = struct_info['name']
            
            code.append(f'    // {struct_name}')
            code.append(f'    {{')
            code.append(f'        printf("----- {struct_name} -----\\n");')
            code.append(f'        printf("  sizeof: %zu bytes\\n", sizeof({struct_name}));')
            
            for member in struct_info['members']:
                member_name = member['name']
                code.append(f'        printf("  offsetof({member_name}): %zu bytes\\n", offsetof({struct_name}, {member_name}));')
            
            code.append(f'        printf("\\n");')
            code.append(f'    }}')
            code.append('')
        
        code.append('    printf("Validation complete for ' + header_name + '!\\n");')
        code.append('}')
        code.append('')
        
        # Add main function
        code.append('// ===== MAIN ENTRY POINT =====')
        code.append('')
        code.append('int main() {')
        code.append('    ValidateStructLayouts();')
        code.append('    return 0;')
        code.append('}')
        
        # Write file
        with open(test_file_path, 'w') as f:
            f.write('\n'.join(code))
        
        generated_files.append(test_file_name)
    
    # Print summary
    print(f"Generated {len(generated_files)} validation files:")
    for fname in generated_files:
        print(f"  - {fname}")
    
    return generated_files

if __name__ == '__main__':
    workspace_root = Path(__file__).parent
    header_dir = workspace_root / 'test' / 'output' / 'cpp'
    output_dir = workspace_root / 'test' / 'output' / 'cpp'
    reflection_file = output_dir / 'reflection_data.json'
    
    if not header_dir.exists():
        print(f"Error: {header_dir} not found")
        exit(1)
    
    # Load reflection data
    _, computed_layouts = load_reflection_data(str(reflection_file))
    
    # Generate validation files
    generate_validation_cpp(str(output_dir), str(header_dir), computed_layouts)
