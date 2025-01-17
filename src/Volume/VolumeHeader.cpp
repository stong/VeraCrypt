/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file)
 and all other portions of this file are Copyright (c) 2013-2017 IDRIX
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages.
*/

#include "Crc32.h"
#include "EncryptionModeXTS.h"
#include "Pkcs5Kdf.h"
#include "Pkcs5Kdf.h"
#include "VolumeHeader.h"
#include "VolumeException.h"
#include "Common/Crypto.h"
#include "Common/Hexdump.h"

#include <stdio.h>

namespace VeraCrypt
{
	VolumeHeader::VolumeHeader (uint32 size)
	{
		Init();
		HeaderSize = size;
		EncryptedHeaderDataSize = size - EncryptedHeaderDataOffset;
	}

	VolumeHeader::~VolumeHeader ()
	{
		Init();
	}

	void VolumeHeader::Init ()
	{
		VolumeKeyAreaCrc32 = 0;
		VolumeCreationTime = 0;
		HeaderCreationTime = 0;
		mVolumeType = VolumeType::Unknown;
		HiddenVolumeDataSize = 0;
		VolumeDataSize = 0;
		EncryptedAreaStart = 0;
		EncryptedAreaLength = 0;
		Flags = 0;
		SectorSize = 0;
	}

	void VolumeHeader::Create (const BufferPtr &headerBuffer, VolumeHeaderCreationOptions &options)
	{
		if (options.DataKey.Size() != options.EA->GetKeySize() * 2 || options.Salt.Size() != GetSaltSize())
			throw ParameterIncorrect (SRC_POS);

		headerBuffer.Zero();

		HeaderVersion = CurrentHeaderVersion;
		RequiredMinProgramVersion = CurrentRequiredMinProgramVersion;

		DataAreaKey.Zero();
		DataAreaKey.CopyFrom (options.DataKey);

		VolumeCreationTime = 0;
		HiddenVolumeDataSize = (options.Type == VolumeType::Hidden ? options.VolumeDataSize : 0);
		VolumeDataSize = options.VolumeDataSize;

		EncryptedAreaStart = options.VolumeDataStart;
		EncryptedAreaLength = options.VolumeDataSize;

		SectorSize = options.SectorSize;

		if (SectorSize < TC_MIN_VOLUME_SECTOR_SIZE
			|| SectorSize > TC_MAX_VOLUME_SECTOR_SIZE
			|| SectorSize % ENCRYPTION_DATA_UNIT_SIZE != 0)
		{
			throw ParameterIncorrect (SRC_POS);
		}

		EA = options.EA;
		shared_ptr <EncryptionMode> mode (new EncryptionModeXTS ());
		EA->SetMode (mode);

		EncryptNew (headerBuffer, options.Salt, options.HeaderKey, options.Kdf);
	}

	bool VolumeHeader::Decrypt (const ConstBufferPtr &encryptedData, const VolumePassword &password, int pim, shared_ptr <Pkcs5Kdf> kdf, bool truecryptMode, const Pkcs5KdfList &keyDerivationFunctions, const EncryptionAlgorithmList &encryptionAlgorithms, const EncryptionModeList &encryptionModes)
	{
		if (password.Size() < 1)
			throw PasswordEmpty (SRC_POS);

		ConstBufferPtr salt (encryptedData.GetRange (SaltOffset, SaltSize));
		Salt.CopyFrom (salt);
		SecureBuffer header (EncryptedHeaderDataSize);
		SecureBuffer headerKey (GetLargestSerializedKeySize());

		foreach (shared_ptr <Pkcs5Kdf> pkcs5, keyDerivationFunctions)
		{
			if (kdf && (kdf->GetName() != pkcs5->GetName()))
				continue;

			pkcs5->DeriveKey (headerKey, password, pim, salt);

			foreach (shared_ptr <EncryptionMode> mode, encryptionModes)
			{
				if (typeid (*mode) != typeid (EncryptionModeXTS))
					mode->SetKey (headerKey.GetRange (0, mode->GetKeySize()));

				foreach (shared_ptr <EncryptionAlgorithm> ea, encryptionAlgorithms)
				{
					if (!ea->IsModeSupported (mode))
						continue;

					if (typeid (*mode) == typeid (EncryptionModeXTS))
					{
						ea->SetKey (headerKey.GetRange (0, ea->GetKeySize()));

						mode = mode->GetNew();
						mode->SetKey (headerKey.GetRange (ea->GetKeySize(), ea->GetKeySize()));
					}
					else
					{
						ea->SetKey (headerKey.GetRange (LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
					}

					ea->SetMode (mode);

					header.CopyFrom (encryptedData.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize));
					ea->Decrypt (header);

					if (Deserialize (header, ea, mode, truecryptMode))
					{
						puts("Decrypt OK.");
						printf("PKCS type: %S\n", pkcs5->GetName().c_str());
						puts("Volume Header dump (SENSITIVE DATA):");
						Hexdump(header, 512-EncryptedHeaderDataOffset);
						puts("Salt:");
						Hexdump(salt, SaltSize);
						SecureBuffer encryptedHeader (EncryptedHeaderDataSize);
						encryptedHeader.CopyFrom (encryptedData.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize));
						puts("Encrypted header (including salt):");
						Hexdump(encryptedData, 512);
						HeaderKey.CopyFrom (headerKey);
						puts("Header key:");
						Hexdump(headerKey, headerKey.Size());

						EA = ea;
						Pkcs5 = pkcs5;
						return true;
					} else {
					}
				}
			}
		}

		return false;
	}

	bool VolumeHeader::Deserialize (const ConstBufferPtr &header, shared_ptr <EncryptionAlgorithm> &ea, shared_ptr <EncryptionMode> &mode, bool truecryptMode)
	{
		if (header.Size() != EncryptedHeaderDataSize)
			throw ParameterIncorrect (SRC_POS);

		if (truecryptMode && (header[0] != 'T' ||
			header[1] != 'R' ||
			header[2] != 'U' ||
			header[3] != 'E'))
			return false;

		if (!truecryptMode && (header[0] != 'V' ||
			header[1] != 'E' ||
			header[2] != 'R' ||
			header[3] != 'A'))
			return false;

		size_t offset = 4;
		HeaderVersion =	DeserializeEntry <uint16> (header, offset);

		if (HeaderVersion < MinAllowedHeaderVersion)
			return false;

		if (HeaderVersion > CurrentHeaderVersion)
			throw HigherVersionRequired (SRC_POS);

		if (HeaderVersion >= 4
			&& Crc32::ProcessBuffer (header.GetRange (0, TC_HEADER_OFFSET_HEADER_CRC - TC_HEADER_OFFSET_MAGIC))
			!= DeserializeEntryAt <uint32> (header, TC_HEADER_OFFSET_HEADER_CRC - TC_HEADER_OFFSET_MAGIC))
		{
			return false;
		}

		RequiredMinProgramVersion = DeserializeEntry <uint16> (header, offset);

		if (!truecryptMode && (RequiredMinProgramVersion > Version::Number()))
			throw HigherVersionRequired (SRC_POS);

		if (truecryptMode)
		{
			if (RequiredMinProgramVersion < 0x600 || RequiredMinProgramVersion > 0x71a)
				throw UnsupportedTrueCryptFormat (SRC_POS);
			RequiredMinProgramVersion = CurrentRequiredMinProgramVersion;
		}

		VolumeKeyAreaCrc32 = DeserializeEntry <uint32> (header, offset);
		VolumeCreationTime = DeserializeEntry <uint64> (header, offset);
		HeaderCreationTime = DeserializeEntry <uint64> (header, offset);
		HiddenVolumeDataSize = DeserializeEntry <uint64> (header, offset);
		mVolumeType = (HiddenVolumeDataSize != 0 ? VolumeType::Hidden : VolumeType::Normal);
		VolumeDataSize = DeserializeEntry <uint64> (header, offset);
		EncryptedAreaStart = DeserializeEntry <uint64> (header, offset);
		EncryptedAreaLength = DeserializeEntry <uint64> (header, offset);
		Flags = DeserializeEntry <uint32> (header, offset);

		printf("VolumeKeyAreaCrc32 = %x\n", VolumeKeyAreaCrc32);
		printf("VolumeCreationTime = %lx\n", VolumeCreationTime);
		printf("HeaderCreationTime = %lx\n", HeaderCreationTime);
		printf("HiddenVolumeDataSize = %lx\n", HiddenVolumeDataSize);
		printf("mVolumeType = %x\n", mVolumeType);
		printf("VolumeDataSize = %lx\n", VolumeDataSize);
		printf("EncryptedAreaStart = %lx\n", EncryptedAreaStart);
		printf("EncryptedAreaLength = %lx\n", EncryptedAreaLength);
		printf("Flags = %x\n", Flags);

		SectorSize = DeserializeEntry <uint32> (header, offset);
		if (HeaderVersion < 5)
			SectorSize = TC_SECTOR_SIZE_LEGACY;

		if (SectorSize < TC_MIN_VOLUME_SECTOR_SIZE
			|| SectorSize > TC_MAX_VOLUME_SECTOR_SIZE
			|| SectorSize % ENCRYPTION_DATA_UNIT_SIZE != 0)
		{
			throw ParameterIncorrect (SRC_POS);
		}

#if !(defined (TC_WINDOWS) || defined (TC_LINUX) || defined (TC_MACOSX))
		if (SectorSize != TC_SECTOR_SIZE_LEGACY)
			throw UnsupportedSectorSize (SRC_POS);
#endif

		offset = DataAreaKeyOffset;

		if (VolumeKeyAreaCrc32 != Crc32::ProcessBuffer (header.GetRange (offset, DataKeyAreaMaxSize)))
			return false;

		DataAreaKey.CopyFrom (header.GetRange (offset, DataKeyAreaMaxSize));

		ea = ea->GetNew();
		mode = mode->GetNew();

		if (typeid (*mode) == typeid (EncryptionModeXTS))
		{
			ea->SetKey (header.GetRange (offset, ea->GetKeySize()));
			mode->SetKey (header.GetRange (offset + ea->GetKeySize(), ea->GetKeySize()));
		}
		else
		{
			mode->SetKey (header.GetRange (offset, mode->GetKeySize()));
			ea->SetKey (header.GetRange (offset + LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
		}

		ea->SetMode (mode);

		return true;
	}

	template <typename T>
	T VolumeHeader::DeserializeEntry (const ConstBufferPtr &header, size_t &offset) const
	{
		offset += sizeof (T);

		if (offset > header.Size())
			throw ParameterIncorrect (SRC_POS);

		return Endian::Big (*reinterpret_cast<const T *> (header.Get() + offset - sizeof (T)));
	}

	template <typename T>
	T VolumeHeader::DeserializeEntryAt (const ConstBufferPtr &header, const size_t &offset) const
	{
		if (offset > header.Size())
			throw ParameterIncorrect (SRC_POS);

		return Endian::Big (*reinterpret_cast<const T *> (header.Get() + offset));
	}

	void VolumeHeader::EncryptNew (const BufferPtr &newHeaderBuffer, const ConstBufferPtr &newSalt, const ConstBufferPtr &newHeaderKey, shared_ptr <Pkcs5Kdf> newPkcs5Kdf)
	{
		if (newHeaderBuffer.Size() != HeaderSize || newSalt.Size() != SaltSize)
			throw ParameterIncorrect (SRC_POS);

		shared_ptr <EncryptionMode> mode = EA->GetMode()->GetNew();
		shared_ptr <EncryptionAlgorithm> ea = EA->GetNew();

		if (typeid (*mode) == typeid (EncryptionModeXTS))
		{
			mode->SetKey (newHeaderKey.GetRange (EA->GetKeySize(), EA->GetKeySize()));
			ea->SetKey (newHeaderKey.GetRange (0, ea->GetKeySize()));
		}
		else
		{
			mode->SetKey (newHeaderKey.GetRange (0, mode->GetKeySize()));
			ea->SetKey (newHeaderKey.GetRange (LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
		}

		ea->SetMode (mode);

		newHeaderBuffer.CopyFrom (newSalt);

		BufferPtr headerData = newHeaderBuffer.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize);
		Serialize (headerData);
		ea->Encrypt (headerData);

		if (newPkcs5Kdf)
			Pkcs5 = newPkcs5Kdf;
	}

	void VolumeHeader::Encrypt (const BufferPtr &newHeaderBuffer) {
		if (newHeaderBuffer.Size() != HeaderSize)
			throw ParameterIncorrect (SRC_POS);

		shared_ptr <EncryptionMode> mode = EA->GetMode()->GetNew();
		shared_ptr <EncryptionAlgorithm> ea = EA->GetNew();

		if (typeid (*mode) == typeid (EncryptionModeXTS))
		{
			mode->SetKey (HeaderKey.GetRange (EA->GetKeySize(), EA->GetKeySize()));
			ea->SetKey (HeaderKey.GetRange (0, ea->GetKeySize()));
		}
		else
		{
			mode->SetKey (HeaderKey.GetRange (0, mode->GetKeySize()));
			ea->SetKey (HeaderKey.GetRange (LegacyEncryptionModeKeyAreaSize, ea->GetKeySize()));
		}

		ea->SetMode (mode);

		newHeaderBuffer.CopyFrom (Salt);

		BufferPtr headerData = newHeaderBuffer.GetRange (EncryptedHeaderDataOffset, EncryptedHeaderDataSize);
		Serialize (headerData);
		ea->Encrypt (headerData);
	}

	size_t VolumeHeader::GetLargestSerializedKeySize ()
	{
		size_t largestKey = EncryptionAlgorithm::GetLargestKeySize (EncryptionAlgorithm::GetAvailableAlgorithms());

		// XTS mode requires the same key size as the encryption algorithm.
		// Legacy modes may require larger key than XTS.
		if (LegacyEncryptionModeKeyAreaSize + largestKey > largestKey * 2)
			return LegacyEncryptionModeKeyAreaSize + largestKey;

		return largestKey * 2;
	}

	void VolumeHeader::Serialize (const BufferPtr &header) const
	{
		if (header.Size() != EncryptedHeaderDataSize)
			throw ParameterIncorrect (SRC_POS);

		header.Zero();

		header[0] = 'V';
		header[1] = 'E';
		header[2] = 'R';
		header[3] = 'A';
		size_t offset = 4;

		header.GetRange (DataAreaKeyOffset, DataAreaKey.Size()).CopyFrom (DataAreaKey);

		uint16 headerVersion = CurrentHeaderVersion;
		SerializeEntry (headerVersion, header, offset);
		SerializeEntry (RequiredMinProgramVersion, header, offset);
		SerializeEntry (Crc32::ProcessBuffer (header.GetRange (DataAreaKeyOffset, DataKeyAreaMaxSize)), header, offset);

		uint64 reserved64 = 0;
		SerializeEntry (reserved64, header, offset);
		SerializeEntry (reserved64, header, offset);

		SerializeEntry (HiddenVolumeDataSize, header, offset);
		SerializeEntry (VolumeDataSize, header, offset);
		SerializeEntry (EncryptedAreaStart, header, offset);
		SerializeEntry (EncryptedAreaLength, header, offset);
		SerializeEntry (Flags, header, offset);

		if (SectorSize < TC_MIN_VOLUME_SECTOR_SIZE
			|| SectorSize > TC_MAX_VOLUME_SECTOR_SIZE
			|| SectorSize % ENCRYPTION_DATA_UNIT_SIZE != 0)
		{
			throw ParameterIncorrect (SRC_POS);
		}

		SerializeEntry (SectorSize, header, offset);

		offset = TC_HEADER_OFFSET_HEADER_CRC - TC_HEADER_OFFSET_MAGIC;
		SerializeEntry (Crc32::ProcessBuffer (header.GetRange (0, TC_HEADER_OFFSET_HEADER_CRC - TC_HEADER_OFFSET_MAGIC)), header, offset);
	}

	template <typename T>
	void VolumeHeader::SerializeEntry (const T &entry, const BufferPtr &header, size_t &offset) const
	{
		offset += sizeof (T);

		if (offset > header.Size())
			throw ParameterIncorrect (SRC_POS);

		*reinterpret_cast<T *> (header.Get() + offset - sizeof (T)) = Endian::Big (entry);
	}

	void VolumeHeader::SetSize (uint32 headerSize)
	{
		HeaderSize = headerSize;
		EncryptedHeaderDataSize = HeaderSize - EncryptedHeaderDataOffset;
	}

	uint32 VolumeHeader::GetSize()
	{
		return HeaderSize;
	}
}
