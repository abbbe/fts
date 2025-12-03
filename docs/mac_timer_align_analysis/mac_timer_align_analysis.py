"""
MAC Timer Alignment Analysis

This module provides functions to analyze MAC clock/timer offset test results.
The firmware generates CSV lines:
    MAC_TIMER_ALIGN,run_id,offset_ticks,offset_ticks_min,offset_ticks_max

Each boot generates a random run_id, makes 100 measurements, then reboots.
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path


def load_mac_timer_align_log(log_path, drop_incomplete_last_run=True):
    """
    Load MAC_TIMER_ALIGN data from log file.

    Args:
        log_path: Path to idf.py stdout log file
        drop_incomplete_last_run: If True, drop the last run if it has < 100 samples

    Returns:
        DataFrame with columns: run_id, offset_ticks, offset_ticks_min, offset_ticks_max
    """
    log_path = Path(log_path)
    if not log_path.exists():
        raise FileNotFoundError(f"Log file not found: {log_path}")

    data = []
    with open(log_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            # Skip header line
            if line.startswith('MAC_TIMER_ALIGN,run,'):
                continue
            if line.startswith('MAC_TIMER_ALIGN,'):
                parts = line.split(',')
                if len(parts) == 5:
                    try:
                        data.append({
                            'run_id': int(parts[1]),
                            'offset_ticks': int(parts[2]),
                            'offset_ticks_min': int(parts[3]),
                            'offset_ticks_max': int(parts[4])
                        })
                    except ValueError:
                        continue  # Skip malformed lines

    df = pd.DataFrame(data)
    if df.empty:
        return df

    # Drop incomplete last run (test was interrupted)
    if drop_incomplete_last_run and len(df) > 0:
        # Get the run_id from the last row (chronologically last run)
        last_run_id = df.iloc[-1]['run_id']
        last_run_count = (df['run_id'] == last_run_id).sum()
        if last_run_count < 100:
            df = df[df['run_id'] != last_run_id]
            print(f"  (Dropped incomplete last run {last_run_id} with {last_run_count} samples)")

    if not df.empty:
        df['range_ticks'] = df['offset_ticks_max'] - df['offset_ticks_min']
        df['offset_us'] = df['offset_ticks'] / 40.0
        df['range_ns'] = df['range_ticks'] * 25.0  # 25ns per tick
    return df


def analyze_chip(log_path, chip_name="Chip"):
    """
    Analyze MAC timer alignment data for a single chip.

    Args:
        log_path: Path to idf.py stdout log file
        chip_name: Name for display purposes

    Returns:
        Dictionary with analysis results
    """
    print(f"\n{'='*60}")
    print(f"  {chip_name}")
    print(f"{'='*60}")

    df = load_mac_timer_align_log(log_path)

    if df.empty:
        print("No MAC_TIMER_ALIGN data found!")
        return {'chip_name': chip_name, 'total_measurements': 0}

    # Basic stats
    total_measurements = len(df)
    num_runs = df['run_id'].nunique()
    print(f"\nTotal measurements: {total_measurements}")
    print(f"Runs: {num_runs}")

    # Measurements per run
    measurements_per_run = df.groupby('run_id').size()
    print(f"\n--- Measurements per run ---")
    print(f"  Mean: {measurements_per_run.mean():.1f}")
    print(f"  Min:  {measurements_per_run.min()}")
    print(f"  Max:  {measurements_per_run.max()}")
    print(f"  Runs with exactly 100: {(measurements_per_run == 100).sum()} / {num_runs}")

    # Offset spread within each run
    run_stats = df.groupby('run_id').agg({
        'offset_ticks': ['nunique'],
        'range_ticks': 'last'  # Final converged range
    }).reset_index()
    run_stats.columns = ['run_id', 'unique_offsets', 'final_range']

    print(f"\n--- Offset convergence within runs ---")
    print(f"  Unique offsets per run:")
    print(f"    Mean: {run_stats['unique_offsets'].mean():.1f}")
    print(f"    Min:  {run_stats['unique_offsets'].min()}")
    print(f"    Max:  {run_stats['unique_offsets'].max()}")

    # Measurement precision (final converged range)
    print(f"\n--- Final converged range per run ---")
    print(f"  Mean: {run_stats['final_range'].mean():.1f} ticks ({run_stats['final_range'].mean()*25:.0f} ns)")
    print(f"  Min:  {run_stats['final_range'].min()} ticks ({run_stats['final_range'].min()*25:.0f} ns)")
    print(f"  Max:  {run_stats['final_range'].max()} ticks ({run_stats['final_range'].max()*25:.0f} ns)")

    # Create figure with subplots (2x2 layout)
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle(f'{chip_name} - MAC Timer Alignment Analysis', fontsize=14)

    # Plot 1: Measurements per run histogram
    ax1 = axes[0, 0]
    measurements_per_run.hist(ax=ax1, bins=20, edgecolor='black')
    ax1.axvline(x=100, color='r', linestyle='--', label='Target (100)')
    ax1.set_xlabel('Measurements per run')
    ax1.set_ylabel('Count (runs)')
    ax1.set_title('Measurements per Run')
    ax1.legend()

    # Plot 2: Unique offsets per run histogram
    ax2 = axes[0, 1]
    run_stats['unique_offsets'].hist(ax=ax2, bins=20, edgecolor='black')
    ax2.set_xlabel('Unique offset values')
    ax2.set_ylabel('Count (runs)')
    ax2.set_title('Unique Offsets per Run')

    # Plot 3: Final converged range per run histogram
    ax3 = axes[1, 0]
    run_stats['final_range'].hist(ax=ax3, bins=30, edgecolor='black')
    ax3.set_xlabel('Final range (ticks)')
    ax3.set_ylabel('Count (runs)')
    ax3.set_title('Final Converged Range per Run')

    # Plot 4: All sample ranges histogram
    ax4 = axes[1, 1]
    df['range_ticks'].hist(ax=ax4, bins=50, edgecolor='black')
    ax4.set_xlabel('Range (ticks)')
    ax4.set_ylabel('Count (samples)')
    ax4.set_title('Sample Range Distribution (all samples)')

    plt.tight_layout()
    plt.show()

    return {
        'chip_name': chip_name,
        'total_measurements': total_measurements,
        'num_runs': num_runs,
        'measurements_per_run': measurements_per_run,
        'run_stats': run_stats,
        'df': df
    }
