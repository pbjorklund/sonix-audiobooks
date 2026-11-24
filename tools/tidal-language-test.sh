#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$root/sonix-packer/assets/R1/usr/resource/sonix/language"
lang_dir=${1:-$source_dir}
for source in "$source_dir"/*.ini; do
	language="$lang_dir/${source##*/}"
	for key in tidal_downloads tidal_download tidal_download_album tidal_download_queued \
		tidal_album_download_queued_note tidal_downloads_finished tidal_downloads_failed_note \
		tidal_retry_failed_downloads tidal_remove_download tidal_download_remove_note \
		tidal_download_progress tidal_album_partial tidal_album_downloaded tidal_download_queue_full \
		tidal_download_missing tidal_saved_for_later tidal_listen_later tidal_remove_saved tidal_saved_failed \
		tidal_tracks tidal_download_playlist tidal_download_quality albums artists playlists; do
		if ! grep -Eq "^${key} = .+" "$language"; then
			printf 'Missing translation %s in %s\n' "$key" "$language" >&2
			exit 1
		fi
	done
done
printf 'TIDAL download labels verified in %s\n' "$lang_dir"
