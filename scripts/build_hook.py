#!/usr/bin/env python3
"""
BeltWinder Matter - Build Hook with Dedicated Generated Directory
"""

import subprocess
import sys
import os
from pathlib import Path

Import("env")

def print_section(title):
    print("\n" + "=" * 70)
    print(f"  {title}")
    print("=" * 70 + "\n")

def get_newest_mtime(files):
    """Gibt den neuesten Timestamp aus einer Liste von Dateien zurück"""
    newest = 0
    for f in files:
        if f.exists():
            mtime = f.stat().st_mtime
            if mtime > newest:
                newest = mtime
    return newest

def main():
    print_section("BeltWinder Matter - Pre-Build: Compressing Web-UI")
    
    # Get project and build directories
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    pioenv = env.subst("$PIOENV")
    
    # ========================================================================
    # WICHTIG: Dediziertes Unterverzeichnis für generierte Dateien!
    # ========================================================================
    generated_dir = build_dir / "generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    
    # Define paths
    input_html = project_dir / "main" / "web_ui.html"
    output_header = generated_dir / "index_html_gz.h"  # ← Im generated/ Unterordner!
    compress_script = project_dir / "scripts" / "compress_ui.py"
    build_hook_script = project_dir / "scripts" / "build_hook.py"
    
    print(f"Project Dir:     {project_dir}")
    print(f"Build Dir:       {build_dir}")
    print(f"Generated Dir:   {generated_dir}")
    print(f"Environment:     {pioenv}")
    print(f"Input HTML:      {input_html}")
    print(f"Output Header:   {output_header}")
    print(f"Compress Script: {compress_script}")
    print("")
    
    # Check if input file exists
    if not input_html.exists():
        print(f"ERROR: web_ui.html not found at {input_html}")
        main_dir = project_dir / "main"
        if main_dir.exists():
            print(f"\nFiles in {main_dir}:")
            for item in sorted(main_dir.iterdir()):
                print(f"  - {item.name}")
        env.Exit(1)
        return
    
    # Check if scripts exist
    if not compress_script.exists():
        print(f"ERROR: compress_ui.py not found at {compress_script}")
        env.Exit(1)
        return
    
    # ========================================================================
    # ROBUST DEPENDENCY CHECK
    # ========================================================================
    
    source_files = [
        input_html,
        compress_script,
        build_hook_script,
    ]
    
    # Check if any source is missing
    missing_sources = [f for f in source_files if not f.exists()]
    if missing_sources:
        print("ERROR: Missing source files:")
        for f in missing_sources:
            print(f"  - {f}")
        env.Exit(1)
        return
    
    # Check if output exists and is up-to-date
    if output_header.exists():
        output_mtime = output_header.stat().st_mtime
        newest_source_mtime = get_newest_mtime(source_files)
        
        print("Dependency Check:")
        print(f"  Output mtime:        {output_mtime:.2f}")
        print(f"  Newest source mtime: {newest_source_mtime:.2f}")
        
        for src in source_files:
            mtime = src.stat().st_mtime
            age_marker = "🔴 NEWER" if mtime > output_mtime else "✓ older"
            print(f"    {src.name:25} {mtime:.2f}  {age_marker}")
        
        print("")
        
        if output_mtime > newest_source_mtime:
            print("✓ Compressed UI is up-to-date - skipping compression")
            print("")
            
            # WICHTIG: Include-Path auch bei Skip hinzufügen!
            print("→ Adding generated directory to include path")
            env.Append(CPPPATH=[str(generated_dir)])
            print(f"  Added: {generated_dir}")
            print("")
            
            print_section("Pre-Build Complete (Skipped)")
            return
        else:
            print("→ Source files have changed - regenerating compressed UI")
    else:
        print("→ Output file doesn't exist - generating compressed UI")
    
    print("")
    
    # ========================================================================
    # RUN COMPRESSION
    # ========================================================================
    
    print("Running UI compression...")
    print("")
    
    try:
        result = subprocess.run(
            [sys.executable, str(compress_script), str(input_html), str(output_header)],
            cwd=str(project_dir),
            check=True,
            capture_output=False,
        )
        
        print_section("Pre-Build Complete - UI Compressed Successfully")
        
        # Verify output was created
        if output_header.exists():
            size = output_header.stat().st_size
            print(f"✓ Output file created: {size:,} bytes")
            
            # Relative path anzeigen
            try:
                rel_path = output_header.relative_to(project_dir)
                print(f"✓ Location: {rel_path}")
            except ValueError:
                print(f"✓ Location: {output_header}")
            
            print("")
        else:
            print("ERROR: Output file not found after compression!")
            env.Exit(1)
            return
        
        # ====================================================================
        # ADD GENERATED DIRECTORY TO INCLUDE PATH
        # ====================================================================
        
        print("→ Adding generated directory to include path")
        env.Append(CPPPATH=[str(generated_dir)])
        print(f"  Added: {generated_dir}")
        print("")
        
    except subprocess.CalledProcessError as e:
        print_section("ERROR: Compression Failed!")
        print(f"Return code: {e.returncode}\n")
        env.Exit(1)
        
    except Exception as e:
        print_section("ERROR: Unexpected Error!")
        print(f"{e}\n")
        import traceback
        traceback.print_exc()
        env.Exit(1)

main()
