#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>

#include "main.h" //for handle_file
#include "epk2.h"
#include "crc.h"
#include "util.h"

EVP_PKEY *_gpPubKey;
AES_KEY _gdKeyImage, _geKeyImage;
const char EPK2_MAGIC[] = "EPK2";
const char EPK3_MAGIC[] = "EPK3";
int fileLength;
unsigned char aes_key[16];

void SWU_CryptoInit_PEM(char *configuration_dir, char *pem_file) {
	OpenSSL_add_all_digests();
	ERR_load_CRYPTO_strings();
	char pem_file_name[1024] = "";
	strcat(pem_file_name, configuration_dir);
	strcat(pem_file_name, "/");
	strcat(pem_file_name, pem_file);
	FILE *pubKeyFile = fopen(pem_file_name, "r");
	if (pubKeyFile == NULL) {
		printf("Error: Can't open PEM file %s\n\n", pem_file);
		exit(1);
	}
	EVP_PKEY *gpPubKey = PEM_read_PUBKEY(pubKeyFile, NULL, NULL, NULL);
	_gpPubKey = gpPubKey;
	if (_gpPubKey == NULL) {
		printf("Error: Can't read PEM signature from file %s\n\n", pem_file);
		fclose(pubKeyFile);
		exit(1);
	}
	fclose(pubKeyFile);
	ERR_clear_error();
}

void SWU_CryptoInit_AES(const unsigned char *AES_KEY) {
	int size = SIGNATURE_SIZE;
	AES_set_decrypt_key(AES_KEY, size, &_gdKeyImage);
	AES_set_encrypt_key(AES_KEY, size, &_geKeyImage);
}

int API_SWU_VerifyImage(unsigned char *image, unsigned int imageSize) {
	unsigned char md_value[0x40];
	unsigned int md_len = 0;
	EVP_MD_CTX ctx1, ctx2;
	EVP_DigestInit(&ctx1, EVP_get_digestbyname("sha1"));
	EVP_DigestUpdate(&ctx1, image + SIGNATURE_SIZE, imageSize - SIGNATURE_SIZE);
	EVP_DigestFinal(&ctx1, (unsigned char *)&md_value, &md_len);
	EVP_DigestInit(&ctx2, EVP_sha1());
	EVP_DigestUpdate(&ctx2, &md_value, md_len);
	int result = 0;
	if (EVP_VerifyFinal(&ctx2, image, SIGNATURE_SIZE, _gpPubKey) == 1)
		result = 1;
	EVP_MD_CTX_cleanup(&ctx1);
	EVP_MD_CTX_cleanup(&ctx2);
	return result;
}

void decryptImage(unsigned char *srcaddr, unsigned int len, unsigned char *dstaddr) {
	unsigned int remaining = len;
	unsigned int decrypted = 0;
	while (remaining >= AES_BLOCK_SIZE) {
		AES_decrypt(srcaddr, dstaddr, &_gdKeyImage);
		srcaddr += AES_BLOCK_SIZE;
		dstaddr += AES_BLOCK_SIZE;
		remaining -= AES_BLOCK_SIZE;
		decrypted++;
	}
	if (remaining != 0) {
		decrypted = decrypted * AES_BLOCK_SIZE;
		memcpy(dstaddr, srcaddr, remaining);
	}
}

void printPAKinfo(struct pak2_t *pak) {
	printf("\nPAK '%.4s' contains %d segment(s):\n", pak->header->name, pak->segment_count);
	int index = 0;
	for (index = 0; index < pak->segment_count; index++) {
		struct pak2segment_t *PAKsegment = pak->segments[index];
		int headerSize = sizeof(struct pak2segmentHeader_t);
		unsigned char *decrypted = calloc(1, headerSize);
		decryptImage(PAKsegment->header->signature, headerSize, decrypted);
		//hexdump(decrypted, headerSize);
		struct pak2segmentHeader_t *decryptedSegmentHeader = (struct pak2segmentHeader_t *)decrypted;
		printf("  segment #%u (name='%.4s', version='%02x.%02x.%02x.%02x', platform='%s', offset='0x%x', size='%u bytes', ", index + 1, pak->header->name, decryptedSegmentHeader->version[3], decryptedSegmentHeader->version[2], decryptedSegmentHeader->version[1], decryptedSegmentHeader->version[0], decryptedSegmentHeader->platform, PAKsegment->content_file_offset, PAKsegment->content_len);
		switch ((build_type_t) decryptedSegmentHeader->devmode) {
		case RELEASE:
			printf("build=RELEASE");
			break;
		case DEBUG:
			printf("build=DEBUG");
			break;
		case TEST:
			printf("build=TEST");
			break;
		default:
			printf("build=UNKNOWN 0x%x\n", decryptedSegmentHeader->devmode);
		}
		printf(")\n");
		free(decrypted);
	}
}

void SelectAESkey(struct pak2_t *pak, struct config_opts_t *config_opts) {
	char key_file_name[1024] = "";
	strcat(key_file_name, config_opts->config_dir);
	strcat(key_file_name, "/");
	strcat(key_file_name, "AES.key");
	FILE *fp = fopen(key_file_name, "r");
	if (fp == NULL) {
		printf("\nError: Cannot open AES.key file.\n\n");
		exit(1);
	}

	int headerSize = sizeof(struct pak2segmentHeader_t);
	unsigned char decrypted[headerSize];
	char *line = NULL;
	size_t len, count;
	ssize_t read;

	while ((read = getline(&line, &len, fp)) != -1) {
		char *pos = line;
		for (count = 0; count < sizeof(aes_key) / sizeof(aes_key[0]); count++) {
			sscanf(pos, "%2hhx", &aes_key[count]);
			pos += 2 * sizeof(char);
		}
		SWU_CryptoInit_AES(aes_key);
		printf("Trying AES key (");
		int i;
		for (i = 0; i < 16; i++)
			printf("%02X", aes_key[i]);
		printf(") for PAK segment decryption...");
		struct pak2segment_t *PAKsegment = pak->segments[0];
		decryptImage(PAKsegment->header->signature, headerSize, (unsigned char *)&decrypted);
		struct pak2segmentHeader_t *decryptedSegmentHeader = (struct pak2segmentHeader_t *)(&decrypted);
		if (!memcmp(decryptedSegmentHeader->pakMagic, "MPAK", 4)) {
			printf("Success!\n");
			fclose(fp);
			if (line)
				free(line);
			return;
		} else {
			printf("Failed\n");
		}
	}
	fclose(fp);
	if (line)
		free(line);
	err_exit("\nFATAL: Can't decrypt PAK. Probably it's decrypted with an unknown key. Aborting now. Sorry.\n\n");
}

int writePAKsegment(struct pak2_t *pak, const char *filename) {
	int length = 0;
	FILE *outfile = fopen(((const char *)filename), "w");
	int index;
	for (index = 0; index < pak->segment_count; index++) {
		struct pak2segment_t *PAKsegment = pak->segments[index];
		int content_len = PAKsegment->content_len;
		unsigned char *decrypted = malloc(content_len);
		memset(decrypted, 0xFF, content_len);
		decryptImage(PAKsegment->content, content_len, decrypted);
		fwrite(decrypted, 1, content_len, outfile);
		length += content_len;
		free(decrypted);
	}
	fclose(outfile);
	return length;
}

int isFileEPK2(const char *epk_file) {
	FILE *file = fopen(epk_file, "rb");
	if (file == NULL) {
		err_exit("Can't open file %s\n\n", epk_file);
	}
	size_t headerSize = 0x650 + SIGNATURE_SIZE;
	unsigned char buffer[headerSize];
	if (fread(&buffer, 1, headerSize, file) != headerSize)
		return 0;
	fclose(file);
	int result = !memcmp(&buffer[0x8C], EPK2_MAGIC, 4);	//old EPK2
	if (!result)
		result = (buffer[0x630 + SIGNATURE_SIZE] == 0 && buffer[0x638 + SIGNATURE_SIZE] == 0x2E && buffer[0x63D + SIGNATURE_SIZE] == 0x2E);	//new EPK2
	return result;
}

int isFileEPK3(const char *epk_file) {
	FILE *file = fopen(epk_file, "rb");
	if (!file)
		err_exit("Can't open file %s\n\n", epk_file);
	size_t headerSize = 0x6BD;
	unsigned char buffer[headerSize];
	if (fread(&buffer, 1, headerSize, file) != headerSize)
		return 0;
	fclose(file);
	int result = (buffer[0x6B0] == 0 && buffer[0x6B5] == 0x2E && buffer[0x6B7] == 0x2E);
	//if (!result) result = (buffer[0x6B0] == 0 && buffer[0x6B8] == 0x2E && buffer[0x6BD] == 0x2E);
	return result;
}

void extractEPK3file(const char *epk_file, struct config_opts_t *config_opts) {
	int file;
	if (!(file = open(epk_file, O_RDONLY))) {
		err_exit("\nCan't open file %s\n\n", epk_file);
	}

	struct stat statbuf;
	if (fstat(file, &statbuf) < 0) {
		err_exit("\nfstat error\n\n");
	}

	int fileLength = statbuf.st_size;
	printf("File size: %d bytes\n", fileLength);
	void *buffer;
	if ((buffer = mmap(0, fileLength, PROT_READ, MAP_SHARED, file, 0)) == MAP_FAILED) {
		err_exit("\nCannot mmap input file. Aborting\n\n");
	}

	printf("\nVerifying digital signature of EPK3 firmware header...\n");
	int verified = 0;
	DIR *dirFile = opendir(config_opts->config_dir);
	if (dirFile) {
		struct dirent *hFile;
		while ((hFile = readdir(dirFile)) != NULL) {
			if (!strcmp(hFile->d_name, ".") || !strcmp(hFile->d_name, "..") || hFile->d_name[0] == '.')
				continue;
			if (strstr(hFile->d_name, ".pem") || strstr(hFile->d_name, ".PEM")) {
				printf("Trying RSA key: %s... ", hFile->d_name);
				SWU_CryptoInit_PEM(config_opts->config_dir, hFile->d_name);
				int size = 0x6B4;
				while (size > SIGNATURE_SIZE) {
					verified = API_SWU_VerifyImage(buffer, size);
					if (verified) {
						printf("Success!\nDigital signature of the firmware is OK. Signed bytes: %d\n\n", size - SIGNATURE_SIZE);
						break;
					}
					size -= 1;
				}
				if (!verified)
					printf("Failed\n");
			}
			if (verified)
				break;
		}
		closedir(dirFile);
	}

	if (!verified) {
		printf("Cannot verify firmware's digital signature (maybe you don't have proper PEM file). Aborting.\n\n");
		if (munmap(buffer, fileLength) == -1)
			printf("Error un-mmapping the file\n\n");
		close(file);
		err_exit("");
	}

	int headerSize = 0x6B4;
	struct epk3header_t *fwInfo = malloc(headerSize);
	memcpy(fwInfo, buffer, headerSize);
	if (memcmp(fwInfo->EPK3magic, EPK3_MAGIC, 4)) {
		printf("Trying to decrypt EPK3 header...\n");
		int uncrypted = 0;
		char key_file_name[1024] = "";
		strcat(key_file_name, config_opts->config_dir);
		strcat(key_file_name, "/");
		strcat(key_file_name, "AES.key");
		FILE *fp = fopen(key_file_name, "r");
		if (fp == NULL) {
			printf("\nError: Cannot open AES.key file.\n\n");
			if (munmap(buffer, fileLength) == -1)
				printf("Error un-mmapping the file\n\n");
			close(file);
			free(fwInfo);
			err_exit("");
		}
		char *line = NULL;
		size_t len = 0;
		ssize_t read;
		size_t count = 0;

		while ((read = getline(&line, &len, fp)) != -1) {
			char *pos = line;
			for (count = 0; count < sizeof(aes_key) / sizeof(aes_key[0]); count++) {
				sscanf(pos, "%2hhx", &aes_key[count]);
				pos += 2 * sizeof(char);
			}
			SWU_CryptoInit_AES(aes_key);
			printf("Trying AES key (%s) ", strtok(line, "\n\r"));
			decryptImage(buffer + SIGNATURE_SIZE, headerSize - SIGNATURE_SIZE, (unsigned char *)fwInfo + SIGNATURE_SIZE);
			if (!memcmp(fwInfo->EPK3magic, EPK3_MAGIC, 4)) {
				printf("Success!\n");
				//hexdump((unsigned char*)fwInfo+SIGNATURE_SIZE, headerSize-SIGNATURE_SIZE);
				uncrypted = 1;
				break;
			} else {
				printf("Failed\n");
			}
		}
		fclose(fp);
		if (line)
			free(line);
		if (!uncrypted) {
			printf("\nFATAL: Cannot decrypt EPK3 header (proper AES key is missing). Aborting now. Sorry.\n\n");
			if (munmap(buffer, fileLength) == -1)
				printf("Error un-mmapping the file");
			close(file);
			free(fwInfo);
			err_exit("");
		}
	}

	printf("\nFirmware info\n");
	printf("-------------\n");
	printf("Firmware magic: %.*s\n", 4, fwInfo->EPK3magic);
	printf("Firmware otaID: %s\n", fwInfo->otaID);
	printf("Firmware version: %02x.%02x.%02x.%02x\n", fwInfo->fwVersion[3], fwInfo->fwVersion[2], fwInfo->fwVersion[1], fwInfo->fwVersion[0]);
	printf("packageInfoSize: %d\n", fwInfo->packageInfoSize);
	printf("bChunked: %d\n", fwInfo->bChunked);

	// Decrypting packageInfo
	struct pak3_t *packageInfo = malloc(fwInfo->packageInfoSize);
	decryptImage(buffer + SIGNATURE_SIZE + 0x654 + SIGNATURE_SIZE, fwInfo->packageInfoSize, (unsigned char *)packageInfo);

	int i;
	FILE *outfile;
	struct pak3segmentHeader_t segment;
	const char *pak_type_name;
	char name[4];

	char fwVersion[1024];
	memset(&fwVersion, 0x0, sizeof(fwVersion));
	sprintf(fwVersion, "%02x.%02x.%02x.%02x-%s", fwInfo->fwVersion[3], fwInfo->fwVersion[2], fwInfo->fwVersion[1], fwInfo->fwVersion[0], fwInfo->otaID);

	sprintf(config_opts->dest_dir, "%s/%s", config_opts->dest_dir, fwVersion);
	createFolder(config_opts->dest_dir);

	long unsigned int offset = SIGNATURE_SIZE + 0x654 + SIGNATURE_SIZE + fwInfo->packageInfoSize;
	for (i = 0; i < packageInfo->numOfSegments; i++) {
		long unsigned int size = 0;
		memcpy(&segment, (unsigned char *)&packageInfo->segment + sizeof(segment) * i, sizeof(segment));
		printf("\nPAK '%s' contains %d segment(s), size %d bytes:\n", segment.name, segment.totalSegments, segment.pakSize);

		char filename[1024] = "";
		sprintf(name, "%s", segment.name);
		sprintf(filename, "%s/%s.pak", config_opts->dest_dir, name);
		printf("Saving partition (%s) to file %s\n", name, filename);
		outfile = fopen(filename, "w");

		int index = 0, realSegmentSize;

		for (index = 0; index < segment.totalSegments; index++) {
			realSegmentSize = segment.segmentSize;
			if (size + realSegmentSize > segment.pakSize)
				realSegmentSize = segment.pakSize - size;

			printf("  segment #%u (name='%s', version='%02x.%02x.%02x.%02x', offset='0x%lx', size='%u bytes')\n", index + 1, segment.name, segment.unknown1[3], segment.unknown1[2], segment.unknown1[1], segment.unknown1[0], offset + SIGNATURE_SIZE, realSegmentSize);

			unsigned char *decrypted = calloc(1, realSegmentSize);
			decryptImage(buffer + offset + SIGNATURE_SIZE, realSegmentSize, decrypted);
			fwrite(decrypted, 1, realSegmentSize, outfile);
			free(decrypted);
			size += realSegmentSize;
			offset += realSegmentSize + SIGNATURE_SIZE;
		}
		fclose(outfile);
		handle_file(filename, config_opts);
		i += index - 1;
	}

	if (munmap(buffer, fileLength) == -1)
		printf("Error un-mmapping the file");
	close(file);
	free(fwInfo);
}

void extractEPK2file(const char *epk_file, struct config_opts_t *config_opts) {
	int file;
	if (!(file = open(epk_file, O_RDONLY))) {
		err_exit("\nCan't open file %s\n\n", epk_file);
	}

	struct stat statbuf;
	if (fstat(file, &statbuf) < 0) {
		err_exit("\nfstat error\n\n");
	}

	int fileLength = statbuf.st_size;
	printf("File size: %d bytes\n", fileLength);
	void *buffer;
	if ((buffer = mmap(0, fileLength, PROT_READ, MAP_SHARED, file, 0)) == MAP_FAILED) {
		err_exit("\nCannot mmap input file. Aborting\n\n");
	}

	printf("\nVerifying digital signature of EPK2 firmware header...\n");
	int verified = 0;
	DIR *dirFile = opendir(config_opts->config_dir);
	if (dirFile) {
		struct dirent *hFile;
		while ((hFile = readdir(dirFile)) != NULL) {
			if (!strcmp(hFile->d_name, ".") || !strcmp(hFile->d_name, "..") || hFile->d_name[0] == '.')
				continue;
			if (strstr(hFile->d_name, ".pem") || strstr(hFile->d_name, ".PEM")) {
				printf("Trying RSA key: %s... ", hFile->d_name);
				SWU_CryptoInit_PEM(config_opts->config_dir, hFile->d_name);
				int size = SIGNATURE_SIZE + 0x634;
				while (size > SIGNATURE_SIZE) {
					verified = API_SWU_VerifyImage(buffer, size);
					if (verified) {
						printf("Success!\nDigital signature of the firmware is OK. Signed bytes: %d\n\n", size - SIGNATURE_SIZE);
						break;
					}
					size -= 1;
				}
				if (!verified)
					printf("Failed\n");
			}
			if (verified)
				break;
		}
		closedir(dirFile);
	}

	if (!verified) {
		printf("Cannot verify firmware's digital signature (maybe you don't have proper PEM file). Aborting.\n\n");
		if (munmap(buffer, fileLength) == -1)
			printf("Error un-mmapping the file\n\n");
		close(file);
		err_exit("");
	}

	int headerSize = 0x5B4 + SIGNATURE_SIZE;
	struct epk2header_t *fwInfo = malloc(headerSize);
	memcpy(fwInfo, buffer, headerSize);
	if (memcmp(fwInfo->EPK2magic, EPK2_MAGIC, 4)) {
		printf("EPK2 header is encrypted. Trying to decrypt...\n");
		int uncrypted = 0;
		char key_file_name[1024] = "";
		strcat(key_file_name, config_opts->config_dir);
		strcat(key_file_name, "/");
		strcat(key_file_name, "AES.key");
		FILE *fp = fopen(key_file_name, "r");
		if (fp == NULL) {
			printf("\nError: Cannot open AES.key file.\n\n");
			if (munmap(buffer, fileLength) == -1)
				printf("Error un-mmapping the file\n\n");
			close(file);
			free(fwInfo);
			err_exit("");
		}
		char *line = NULL;
		size_t len = 0;
		ssize_t read;
		size_t count = 0;

		while ((read = getline(&line, &len, fp)) != -1) {
			char *pos = line;
			for (count = 0; count < sizeof(aes_key) / sizeof(aes_key[0]); count++) {
				sscanf(pos, "%2hhx", &aes_key[count]);
				pos += 2 * sizeof(char);
			}
			SWU_CryptoInit_AES(aes_key);
			printf("Trying AES key (%s) ", strtok(line, "\n\r"));
			decryptImage(buffer + SIGNATURE_SIZE, headerSize - SIGNATURE_SIZE, (unsigned char *)fwInfo + SIGNATURE_SIZE);
			//printf("\n"); hexdump((unsigned char*)fwInfo+SIGNATURE_SIZE, headerSize-SIGNATURE_SIZE);
			if (!memcmp(fwInfo->EPK2magic, EPK2_MAGIC, 4)) {
				printf("Success!\n");
				uncrypted = 1;
				break;
			} else {
				printf("Failed\n");
			}
		}
		fclose(fp);
		if (line)
			free(line);
		if (!uncrypted) {
			printf("\nFATAL: Cannot decrypt EPK2 header (proper AES key is missing). Aborting now. Sorry.\n\n");
			if (munmap(buffer, fileLength) == -1)
				printf("Error un-mmapping the file");
			close(file);
			free(fwInfo);
			err_exit("");
		}
	}

	printf("\nFirmware info\n");
	printf("-------------\n");
	printf("Firmware magic: %.*s\n", 4, fwInfo->EPK2magic);
	printf("Firmware otaID: %s\n", fwInfo->otaID);
	printf("Firmware version: %02x.%02x.%02x.%02x\n", fwInfo->fwVersion[3], fwInfo->fwVersion[2], fwInfo->fwVersion[1], fwInfo->fwVersion[0]);
	printf("PAK count: %d\n", fwInfo->pakCount);
	printf("PAKs total size: %d\n", fwInfo->fileSize);
	printf("Header length: %d\n\n", fwInfo->headerLength);

	struct epk2header_t *fwFile = (struct epk2header_t *)buffer;
	struct pak2_t **pakArray = malloc((fwInfo->pakCount) * sizeof(struct pak2_t *));
	if (fileLength < fwInfo->fileSize)
		printf("\n!!!WARNING: Real file size is shorter than file size listed in the header. Number of extracted PAKs will be lowered to filesize...\n");

	printf("\nScanning EPK2 firmware...\n");
	// Scan PAK segments
	unsigned char *epk2headerOffset = fwFile->signature;
	unsigned char *pak2headerOffset = fwInfo->signature + sizeof(struct epk2header_t);
	struct pak2segmentHeader_t *pak2segmentHeader = (struct pak2segmentHeader_t *)((fwFile->epakMagic) + (fwInfo->headerLength));

	// Contains added lengths of signature data
	unsigned int signature_sum = sizeof(fwFile->signature) + sizeof(pak2segmentHeader->signature);
	unsigned int pak2segmentHeaderSignatureLength = sizeof(pak2segmentHeader->signature);
	int count = 0;
	int next_pak_length = fwInfo->fileSize;

	while (count < fwInfo->pakCount) {
		struct pak2header_t *pakHeader = (struct pak2header_t *)(pak2headerOffset);
		struct pak2_t *pak = malloc(sizeof(struct pak2_t));
		pakArray[count] = pak;
		pak->header = pakHeader;
		pak->segment_count = 0;
		pak->segments = NULL;
		int verified = 0;
		struct pak2segmentHeader_t *next_pak_offset = (struct pak2segmentHeader_t *)
			(epk2headerOffset + pakHeader->nextPAKfileOffset + signature_sum);
		unsigned int distance_between_paks = (next_pak_offset->name) - (pak2segmentHeader->name);

		// Last contained PAK...
		if (count == (fwInfo->pakCount - 1))
			distance_between_paks = next_pak_length + pak2segmentHeaderSignatureLength;
		unsigned int max_distance = pakHeader->maxPAKsegmentSize + sizeof(struct pak2segmentHeader_t);
		while (!verified) {
			unsigned int PAKsegment_length = distance_between_paks;
			bool is_next_segment_needed = false;
			if (PAKsegment_length > max_distance) {
				PAKsegment_length = max_distance;
				is_next_segment_needed = true;
			}
			unsigned int signed_length = next_pak_length;
			if (signed_length > max_distance) {
				signed_length = max_distance;
			}
			if (count == 0)
				signed_length = PAKsegment_length;

			if (!verified && (verified = API_SWU_VerifyImage(pak2segmentHeader->signature, signed_length)) != 1) {
				printf("Verification of the PAK segment #%u failed (size=0x%X). Trying to fallback...\n", pak->segment_count + 1, signed_length);
				while (((verified = API_SWU_VerifyImage(pak2segmentHeader->signature, signed_length)) != 1) && (signed_length > 0))
					signed_length--;
				if (verified)
					printf("Successfully verified with size: 0x%X\n", signed_length);
				else {
					printf("Fallback failed. Sorry, aborting now.\n\n");
					if (munmap(buffer, fileLength) == -1)
						printf("Error un-mmapping the file\n\n");
					close(file);
					free(fwInfo);
					int i;
					for (i = 0; i < count; ++i)
						free(pakArray[i]);
					free(pakArray);
					err_exit("");
				}
			}
			// Sum signature lengths
			signature_sum += pak2segmentHeaderSignatureLength;
			unsigned int PAKsegment_content_length = (PAKsegment_length - pak2segmentHeaderSignatureLength);

			if (is_next_segment_needed) {
				distance_between_paks -= PAKsegment_content_length;
				next_pak_length -= PAKsegment_content_length;
				verified = 0;
			} else
				next_pak_length = pakHeader->nextPAKlength + pak2segmentHeaderSignatureLength;

			pak->segment_count++;
			pak->segments = realloc(pak->segments, pak->segment_count * sizeof(struct pak2segment_t *));
			struct pak2segment_t *PAKsegment = malloc(sizeof(struct pak2segment_t));
			PAKsegment->header = pak2segmentHeader;
			PAKsegment->content = pak2segmentHeader->signature + sizeof(struct pak2segmentHeader_t);
			PAKsegment->content_file_offset = PAKsegment->content - epk2headerOffset;
			PAKsegment->content_len = signed_length - sizeof(struct pak2segmentHeader_t);
			pak->segments[pak->segment_count - 1] = PAKsegment;

			// Move pointer to the next pak segment offset
			pak2segmentHeader = (struct pak2segmentHeader_t *)(pak2segmentHeader->signature + PAKsegment_length);
		}
		pak2headerOffset += sizeof(struct pak2header_t);
		count++;

		// File truncation check
		if ((pakHeader->nextPAKfileOffset + next_pak_length) > fileLength)
			break;
	}

	int last_index = count - 1;

	struct pak2_t *last_pak = pakArray[last_index];
	int PAKsegment_index = last_pak->segment_count - 1;

	struct pak2segment_t *last_PAKsegment = last_pak->segments[PAKsegment_index];
	int last_extracted_file_offset = (last_PAKsegment->content_file_offset + last_PAKsegment->content_len);
	printf("Last extracted file offset: %d\n\n", last_extracted_file_offset);

	char fwVersion[1024];
	memset(&fwVersion, 0x0, sizeof(fwVersion));
	sprintf(fwVersion, "%02x.%02x.%02x.%02x-%s", fwInfo->fwVersion[3], fwInfo->fwVersion[2], fwInfo->fwVersion[1], fwInfo->fwVersion[0], fwInfo->otaID);
	sprintf(config_opts->dest_dir, "%s/%s", config_opts->dest_dir, fwVersion);
	createFolder(config_opts->dest_dir);

	SelectAESkey(pakArray[0], config_opts);

	int index;
	for (index = 0; index < last_index + 1; index++) {
		printPAKinfo(pakArray[index]);
		const char *pak_type_name;
		char filename[1024] = "";
		char name[4];
		sprintf(name, "%.4s", pakArray[index]->header->name);
		sprintf(filename, "%s/%.4s.pak", config_opts->dest_dir, name);
		printf("#%u/%u saving PAK (%s) to file %s\n", index + 1, fwInfo->pakCount, name, filename);
		int length = writePAKsegment(pakArray[index], filename);
		free(pakArray[index]);
		handle_file(filename, config_opts);
	}
	if (munmap(buffer, fileLength) == -1)
		printf("Error un-mmapping the file");
	close(file);
	free(fwInfo);
	free(pakArray);
}
