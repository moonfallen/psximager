//
// PSXRip - Disassemble a PlayStation 1 disc image into its contents
//
// Copyright (C) 2014 Christian Bauer <www.cebix.net>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
//

//
// std namespace
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

//
// boost namespace
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
// using boost::format;

#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/info.h>
#include <libvcd/sector.h>
}
// end includes

// Sector buffer
static char buffer[M2RAW_SECTOR_SIZE];

// Print an ISO long-format time structure to a file.
static void print_ltime(
  std::ofstream & f, 
  const iso9660_ltime_t & l
) {
	f << boost::format("%.4s-%.2s-%.2s %.2s:%.2s:%.2s.%.2s %d")
	   % l.lt_year % l.lt_month % l.lt_day
	   % l.lt_hour % l.lt_minute % l.lt_second % l.lt_hsecond
	   % int(l.lt_gmtoff) << std::endl;
}
//
// end print_ltime()

// Dump system area data from image to file.
static void dumpSystemArea(
  CdIo_t * image, 
  const boost::filesystem::path & fileName
) {
	std::ofstream file(fileName.string(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!file) {
		throw std::runtime_error((boost::format("Cannot create system area file %1%\n") % fileName).str());
	}

	const size_t numSystemAreaSectors = 16;
	for (size_t sector = 0; sector < numSystemAreaSectors; ++sector) {
		driver_return_code_t r = cdio_read_mode2_sector(image, buffer, sector, true);
		if (r != DRIVER_OP_SUCCESS) {
			throw std::runtime_error((boost::format("Error reading sector %1% of image file: %2%" ) % sector % cdio_driver_errmsg(r) ).str());
		}

		if (buffer[2] != SM_DATA) {

			// End of data
			break;
		}

		file.write(buffer + CDIO_CD_SUBHEADER_SIZE, CDIO_CD_FRAMESIZE);
		if (!file) {
			throw std::runtime_error((boost::format("Cannot write to system area file %1%") % fileName).str());
		}
	}
}

// Functor for sorting a container of iso9660_stat_t pointers by LSN
struct CmpByLSN {
	bool operator()(const iso9660_stat_t * lhs, const iso9660_stat_t * rhs)
	{
		return lhs->lsn < rhs->lsn;
	}
};

// Recursively dump the contents of the ISO filesystem starting at 'dir'
// while extending the catalog file.
static void dumpFilesystem(
  CdIo_t * image, 
  std::ofstream & catalog, 
  bool writeLBNs,
  const boost::filesystem::path & outputPath, 
  const std::string & inputPath = "",
  const std::string & dirName = "", 
  unsigned level = 0
) {
	cdio_info("Dumping '%s' as '%s'", inputPath.c_str(), dirName.c_str());

	// Read the directory entries
	CdioList_t * entries = iso9660_fs_readdir(image, inputPath.c_str());
	if (!entries) {
		throw std::runtime_error((boost::format("Error reading ISO 9660 directory '%1%'") % inputPath).str());
	}

	// Create the output directory
	boost::filesystem::path outputDirName = outputPath / dirName;
	boost::filesystem::create_directory(outputDirName);

	// Open the catalog record for the directory
	if (level == 0) {
		catalog << "dir {\n";  // root
	} else {
		catalog << std::string(level * 2, ' ') << "dir " << dirName;
		if (writeLBNs) {
			iso9660_stat_t * stat = static_cast<iso9660_stat_t *>(_cdio_list_node_data(_cdio_list_begin(entries)));  // "." entry
			catalog << " @" << stat->lsn;
		}
		catalog << " {\n";
	}

	// Sort entries by sector number
	std::vector<iso9660_stat_t *> sortedChildren;

	CdioListNode_t * entry;
	_CDIO_LIST_FOREACH(entry, entries) {
		sortedChildren.push_back(static_cast<iso9660_stat_t *>(_cdio_list_node_data(entry)));
	}

	std::sort(sortedChildren.begin(), sortedChildren.end(), CmpByLSN());

	// Dump all entries
	for (std::vector<iso9660_stat_t *>::const_iterator i = sortedChildren.begin(); i != sortedChildren.end(); ++i) {
		iso9660_stat_t * stat = *i;
		std::string entryName = stat->filename;
		std::string entryPath = inputPath.empty() ? entryName : (inputPath + "/" + entryName);

		if (stat->type == iso9660_stat_s::_STAT_DIR) {

			// Entry is a directory, recurse into it unless it is "." or ".."
			if (entryName != "." && entryName != "..") {
				dumpFilesystem(image, catalog, writeLBNs, outputDirName, entryPath, entryName, level + 1);
			}

		} else {

			// Entry is a file, strip the version number
			size_t versionSep = entryName.find_last_of(';');
			if (versionSep != std::string::npos) {
				entryName = entryName.substr(0, versionSep);
			}

			// Is it an XA form 2 file?
			bool form2File = false;
			if (stat->b_xa) {
				uint16_t attr = uint16_from_be(stat->xa.attributes);
				if (attr & (XA_ATTR_MODE2FORM2 | XA_ATTR_INTERLEAVED)) {
					cdio_info("XA file '%s' size = %u, secsize = %u, group_id = %d, user_id = %d, attributes = %04x, filenum = %d",
					          entryName.c_str(), stat->size, stat->secsize, stat->xa.group_id, stat->xa.user_id, attr, stat->xa.filenum);
					form2File = true;
				}

				if (attr & XA_ATTR_CDDA) {
					std::cout << "Skipping '" << entryPath << "' which is a CD-DA file\n";
					continue;
				}
			}

			// For form 2 files, the size in the directory record is usually just
			// the ISO block size (2048) times the number of sectors in the file.
			// The actual file size is larger because the sectors have 2336 bytes.
			size_t blockSize = form2File ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
			size_t fileSize = form2File ? stat->secsize * blockSize : stat->size;

			// Write the catalog record for the file
			catalog << std::string((level + 1) * 2, ' ') << (form2File ? "xa" : "") << "file " << entryName;
			if (writeLBNs) {
				catalog << " @" << stat->lsn;
			}
			catalog << "\n";

			// Dump the file contents
			boost::filesystem::path outputFileName = outputDirName / entryName;
			std::ofstream file(outputFileName.string(), std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
			if (!file) {
				throw std::runtime_error((boost::format("Cannot create output file %1%") % outputFileName).str());
			}

			size_t sizeRemaining = fileSize;

			for (lsn_t sector = 0; sector < stat->secsize; ++sector) {
				memset(buffer, 0, blockSize);

				driver_return_code_t r;
				if (form2File) {
					r = cdio_read_mode2_sector(image, buffer, stat->lsn + sector, true);
				} else {
					r = cdio_read_data_sectors(image, buffer, stat->lsn + sector, blockSize, 1);
				}
				if (r != DRIVER_OP_SUCCESS) {
					std::cerr << boost::format("Error reading sector %1% of image file: %2%") % (stat->lsn + sector) % cdio_driver_errmsg(r) << std::endl;
					std::cerr << boost::format("Output file %1% may be incomplete") % outputFileName << std::endl;
					break;
				}

				size_t sizeToWrite = sizeRemaining > blockSize ? blockSize : sizeRemaining;

				file.write(buffer, sizeToWrite);
				if (!file) {
					throw std::runtime_error((boost::format("Cannot write to file %1%") % outputFileName).str());
				}

				sizeRemaining -= sizeToWrite;
			}
		}
	}

	// Close the catalog record for the directory
	catalog << std::string(level * 2, ' ') << "}\n";

	_cdio_list_free(entries, true);
}
// end dumpFilesystem()
//

// Dump image to system area data, catalog file, and output directory.
static void dumpImage(
  CdIo_t * image, 
  const boost::filesystem::path & outputPath, 
  bool writeLBNs
) {
	// Read ISO volume information
	iso9660_pvd_t pvd;
	if (!iso9660_fs_read_pvd(image, &pvd)) {
		throw std::runtime_error("Error reading ISO 9660 volume information");
	}
	std::cout << "Volume ID = " << iso9660_get_volume_id(&pvd) << std::endl;

	// Construct names of output files
	boost::filesystem::path catalogName = outputPath;
	catalogName.replace_extension(".cat");

	boost::filesystem::path systemAreaName = outputPath;
	systemAreaName.replace_extension(".sys");

	// Create output catalog file
	std::ofstream catalog(catalogName.string(), std::ofstream::out | std::ofstream::trunc);
	if (!catalog) {
		throw std::runtime_error((boost::format("Cannot create catalog file %1%") % catalogName).str());
	}

	// Dump system area data
	dumpSystemArea(image, systemAreaName);

	std::cout << "System area data written to " << systemAreaName << "\n";

	catalog << "system_area {\n";
	catalog << "  file " << systemAreaName << "\n";
	catalog << "}\n\n";

	// Output ISO volume information
	catalog << "volume {\n";
	catalog << "  system_id [" << iso9660_get_system_id(&pvd) << "]\n";
	catalog << "  volume_id [" << iso9660_get_volume_id(&pvd) << "]\n";
	catalog << "  volume_set_id [" << iso9660_get_volumeset_id(&pvd) << "]\n";
	catalog << "  publisher_id [" << iso9660_get_publisher_id(&pvd) << "]\n";
	catalog << "  preparer_id [" << iso9660_get_preparer_id(&pvd) << "]\n";
	catalog << "  application_id [" << iso9660_get_application_id(&pvd) << "]\n";
	catalog << "  copyright_file_id [" << vcdinfo_strip_trail(pvd.copyright_file_id, 37) << "]\n";
	catalog << "  abstract_file_id [" << vcdinfo_strip_trail(pvd.abstract_file_id, 37) << "]\n";
	catalog << "  bibliographic_file_id [" << vcdinfo_strip_trail(pvd.bibliographic_file_id, 37) << "]\n";
	catalog << "  creation_date "; print_ltime(catalog, pvd.creation_date);
	catalog << "  modification_date "; print_ltime(catalog, pvd.modification_date);
	catalog << "  expiration_date "; print_ltime(catalog, pvd.expiration_date);
	catalog << "  effective_date "; print_ltime(catalog, pvd.effective_date);
	catalog << "}\n\n";

	// Dump ISO filesystem
	if (!iso9660_fs_read_superblock(image, ISO_EXTENSION_NONE)) {
		throw std::runtime_error("Error reading ISO 9660 volume information");
	}

	std::cout << "Dumping filesystem to directory " << outputPath << "...\n";
	dumpFilesystem(image, catalog, writeLBNs, outputPath);

	// Close down
	std::cout << "Catalog written to " << catalogName << "\n";
}
//
// end dumpImage()


// Main program
void psxrip(
  boost::filesystem::path inputPath
) {
	// Parse command line arguments
  boost::filesystem::path outputPath;
	bool writeLBNs = false;
  cdio_loglevel_default = CDIO_LOG_INFO;

	if (outputPath.empty()) {
		outputPath = inputPath;
		outputPath.replace_extension("");
	}

	try {
    //
		// Open the input image
 		if (inputPath.extension().empty()) {
			inputPath.replace_extension(".bin");
		}

		CdIo_t * image = cdio_open(inputPath.string(), DRIVER_BINCUE);
		
    if (image == NULL) {
			throw std::runtime_error((boost::format("Error opening input image %1%, or image has wrong type") % inputPath).str());
		}

		std::cout << "Analyzing image " << inputPath << "...\n";

		// Is it the correct type?
		discmode_t discMode = cdio_get_discmode(image);
		cdio_info("Disc mode = %d", discMode);
		switch (discMode) {
			case CDIO_DISC_MODE_CD_DATA:
			case CDIO_DISC_MODE_CD_XA:
			case CDIO_DISC_MODE_CD_MIXED:
				break;
			default:
				throw std::runtime_error("Input image is not a CD-ROM data disc");
		}

		track_t firstTrack = cdio_get_first_track_num(image);
		cdio_info("First track = %d", firstTrack);

		if (firstTrack == CDIO_INVALID_TRACK) {
			throw std::runtime_error("Cannot determine first track number");
		}

		track_format_t trackFormat = cdio_get_track_format(image, firstTrack);
		cdio_info("Track format = %d", trackFormat);

		if (trackFormat != TRACK_FORMAT_DATA && trackFormat != TRACK_FORMAT_XA) {
			throw std::runtime_error((boost::format("First track (%1%) is not a data track") % firstTrack).str());
		}

		msf_t startMSF;
		cdio_get_track_msf(image, firstTrack, &startMSF);
		lsn_t startLSN = firstTrack == 1 ? 0 : cdio_msf_to_lsn(&startMSF);
		cdio_info("Start LSN of session = %d", startLSN);

		cdio_fs_anal_t type;
		cdio_iso_analysis_t isoType;
		type = cdio_guess_cd_type(image, startLSN, firstTrack, &isoType);
		cdio_info("Filesystem type = %04x", type);

		if (CDIO_FSTYPE(type) != CDIO_FS_ISO_9660) {
			throw std::runtime_error("No ISO 9660 filesystem on data track");
		}


    // Dump the input image
    dumpImage(image, outputPath, writeLBNs);

		// Close the input image
		cdio_destroy(image);
		cdio_info("Done.");

	} catch (const std::exception & e) {
		std::cerr << e.what() << std::endl;
	}
}
