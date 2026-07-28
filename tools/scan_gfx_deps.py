import os
import sys

# The specific graphic API headers to hunt down
TARGET_INCLUDES = [
    "#include <bgfx/bgfx.h>",
    "#include <bx/math.h>",
    "#include <bgfx/platform.h>",
    "#include <bx/allocator.h>"
]

# Standard C/C++ extensions to scan
CPP_EXTENSIONS = ('.h', '.hpp', '.inl', '.c', '.cpp', '.cc', '.cxx')

def scan_runtime_folder(root_dir):
    dirty_files = []
    
    if not os.path.exists(root_dir):
        print(f"❌ Error: The path '{root_dir}' does not exist.")
        return

    print(f"🔍 Auditing layout dependencies in: {root_dir}...")
    
    # Recursively crawl through the directory tree
    for root, _, files in os.walk(root_dir):
        for file in files:
            if file.endswith(CPP_EXTENSIONS):
                file_path = os.path.join(root, file)
                
                try:
                    # open with utf-8 and ignore errors to handle any weird characters gracefully
                    with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                        content = f.read()
                        
                        # Check if any of our target graphics includes are present
                        found_deps = [inc for inc in TARGET_INCLUDES if inc in content]
                        
                        if found_deps:
                            dirty_files.append((file_path, found_deps))
                except Exception as e:
                    print(f"⚠️ Could not read {file_path}: {e}")

    # Output the final status report
    if not dirty_files:
        print("\n✨ all good ✨")
        print("No rendering backend dependencies leaked into this folder layer!")
    else:
        print(f"\n🚨 Found {len(dirty_files)} files leaking graphics API dependencies:\n")
        for path, deps in dirty_files:
            print(f"📁 {os.path.relpath(path, root_dir)}")
            for dep in deps:
                print(f"   ↳ Used: {dep}")
            print("-" * 50)

if __name__ == "__main__":
    # Pull directory path from arguments, or default to the current working directory
    target_path = sys.argv[1] if len(sys.argv) > 1 else "./runtime"
    scan_runtime_folder(target_path)