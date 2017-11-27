#include "Processor.h"

#include <cassert>
#include <iostream>

#include "common/FileUtil.h"

std::string const Processor::bootImagePath = "resources/rpcboot.bin";
unsigned const Processor::bootImageOffset = 1024;
unsigned const Processor::bootImageSize = 256;
unsigned const Processor::cyclesPerTick = 1000;

Processor::Processor(RedbusNetwork & network, unsigned memoryBanks, uint8_t address) :
	RedbusDevice(network, address),
	memory(),
	memoryBanks(memoryBanks),
	regs{0, 0, 0, 0, 0, 0, 0, 0, 0},
	mmu{0, 0, 0, false, false},
	flags(0),
	brkAddress(8192),
	porAddress(8192),
	ticks(0),
	remainingCycles(0),
	isRunning(false),
	rbTimeout(false),
	waiTimeout(false),
	rbCache(nullptr)
{
	assert(this->memoryBanks != 0);
	assert(this->memoryBanks <= maxBankCount);

	coldBoot();
}

void Processor::coldBoot()
{
	brkAddress = porAddress = 8192;
	regs.SP = 512;
	regs.PC = 1024;
	regs.R = 768;

	regs.A = regs.X = regs.Y = regs.D = 0;
	flags = 0;
	setFlag(FlagE);
	setFlag(FlagM);
	setFlag(FlagX);

	std::cout << "Cold boot flags: " << flags << std::endl;

	memory[0] = 2; // Disk
	memory[1] = 1; // Console

	loadBootImage();

	remainingCycles = 0;
	isRunning = false;
}

void Processor::warmBoot()
{
	if (isRunning) {
		regs.SP = 512;
		regs.R = 768;
		regs.PC = porAddress;
	}

	remainingCycles = 0;
	isRunning = true;
}

void Processor::halt()
{
	isRunning = false;
}

void Processor::runTick()
{
	++ticks;

	if (!isRunning) {
		return;
	}

	rbCache = nullptr;
	rbTimeout = false;
	waiTimeout = false;

	remainingCycles += cyclesPerTick;
	if (remainingCycles > 100 * cyclesPerTick) {
		remainingCycles = 100 * cyclesPerTick;
	}

	while (isRunning
		&& remainingCycles-- > 0
		&& !waiTimeout
		&& !rbTimeout)
	{
		processInstruction();
	}
}

uint8_t Processor::read(uint8_t address)
{
	if (!mmu.externalWindowEnabled) {
		return 0;
	}

	return readOnlyMemory(mmu.externalWindow + address);
}

void Processor::write(uint8_t address, uint8_t value)
{
	if (!mmu.externalWindowEnabled) {
		return;
	}

	writeOnlyMemory(mmu.externalWindow + address, value);
}

void Processor::setFlags(uint8_t mask)
{
	bool flagM = getFlag(FlagM);
	flags = mask | (flags & 0xff00);

	if (getFlag(FlagE)) {
		clearFlag(FlagX);
		clearFlag(FlagM);
	} else {
		if (getFlag(FlagX)) {
			regs.X &= 0xff;
			regs.Y &= 0xff;
		}
		if (getFlag(FlagM) != flagM) {
			if (getFlag(FlagM)) {
				regs.B = regs.A >> 8;
				regs.A &= 0xff;
			} else {
				regs.A |= regs.B << 8;
			}
		}
	}
}

void Processor::resetFlags(uint8_t mask)
{
	uint8_t baseFlags = flags & 0xff;
	baseFlags &= ~mask;

	setFlags(baseFlags);
}

void Processor::setFlag(Flag flag)
{
	flags |= flag;
}

void Processor::clearFlag(Flag flag)
{
	flags &= ~flag;
}

bool Processor::getFlag(Flag flag)
{
	return flags & flag;
}

uint8_t Processor::readOnlyMemory(uint16_t address)
{
	if ((address >> 13) + 1u > memoryBanks) {
		return 255;
	}

	return memory[address];
}

uint8_t Processor::readMemory(uint16_t address)
{
	if (mmu.redbusEnabled
		&& (address >= mmu.redbusWindow
			&& address < (mmu.redbusWindow + 256)))
	{
		std::cout << "Reading from RedBus at " << address << std::endl;
		if (rbCache == nullptr) {
			rbCache = RedbusDevice::findDevice(mmu.redbusAddress);
		}
		if (rbCache == nullptr) {
			std::cout << "Device " << +mmu.redbusAddress << " is not found on Redbus!" << std::endl;
			rbTimeout = true;
			return 0;
		}

		uint8_t tmp = rbCache->read(address - mmu.redbusWindow);
		std::cout << "Readed: " << +tmp << std::endl;
		return tmp;
	}

	return readOnlyMemory(address);
}

void Processor::writeOnlyMemory(uint16_t address, uint8_t value)
{
	if ((address >> 13) + 1u > memoryBanks) {
		return;
	}

	memory[address] = value;
}

void Processor::writeMemory(uint16_t address, uint8_t value)
{
	if (mmu.redbusEnabled
		&& (address >= mmu.redbusWindow
			&& address < (mmu.redbusWindow + 256)))
	{
		std::cout << "Writing " << +value << " to RedBus at " << address << std::endl;
		if (rbCache == nullptr) {
			rbCache = RedbusDevice::findDevice(mmu.redbusAddress);
		}
		if (rbCache == nullptr) {
			std::cout << "Device " << +mmu.redbusAddress << " is not found on Redbus!" << std::endl;
			rbTimeout = true;
			return;
		}

		rbCache->write(address - mmu.redbusWindow, value);
	}

	writeOnlyMemory(address, value);
}

uint8_t Processor::readByte()
{
	return readMemory(regs.PC++);
}

uint16_t Processor::readM()
{
	uint16_t i = readByte();
	if (!getFlag(FlagM)) {
		i |= readByte() << 8;
	}
	return i;
}

uint16_t Processor::readX()
{
	uint16_t i = readByte();
	if (!getFlag(FlagX)) {
		i |= readByte() << 8;
	}
	return i;
}

uint16_t Processor::readM(uint16_t address)
{
	uint16_t i = readMemory(address);
	if (!getFlag(FlagM)) {
		i |= readMemory(address + 1) << 8;
	}
	return i;
}

uint16_t Processor::readX(uint16_t address)
{
	uint16_t i = readMemory(address);
	if (!getFlag(FlagX)) {
		i |= readMemory(address + 1) << 8;
	}
	return i;
}

void Processor::writeM(uint16_t address, uint16_t value)
{
	writeMemory(address, value & 0xff);
	if (!getFlag(FlagM)) {
		writeMemory(address + 1, value >> 8);
	}
}

void Processor::writeX(uint16_t address, uint16_t value)
{
	writeMemory(address, value & 0xff);
	if (!getFlag(FlagX)) {
		writeMemory(address + 1, value >> 8);
	}
}

uint16_t Processor::readBX()
{
	uint16_t i = readByte() + regs.X;
	if (getFlag(FlagX)) {
		i &= 0xff;
	}
	return i;
}

uint16_t Processor::readBY()
{
	uint16_t i = readByte() + regs.Y;
	if (getFlag(FlagX)) {
		i &= 0xff;
	}
	return i;
}

uint16_t Processor::readBS()
{
	return readByte() + regs.SP;
}

uint16_t Processor::readBR()
{
	return readByte() + regs.R;
}

uint16_t Processor::readBSWY()
{
	uint16_t i = readByte() + regs.R;
	return readW(i) + regs.Y;
}

uint16_t Processor::readW()
{
	uint16_t i = readByte();
	i |= readByte() << 8;
	return i;
}

uint16_t Processor::readW(uint16_t address)
{
	uint16_t i = readMemory(address);
	i |= readMemory(address + 1) << 8;
	return i;
}

uint16_t Processor::readWX()
{
	uint16_t i = readByte();
	i |= readByte() << 8;
	return i + regs.X;
}

uint16_t Processor::readWY()
{
	uint16_t i = readByte();
	i |= readByte() << 8;
	return i + regs.Y;
}

uint16_t Processor::readWXW()
{
	return readW(readWX());
}

uint16_t Processor::readBW()
{
	return readW(readByte());
}

uint16_t Processor::readWW()
{
	return readW(readW());
}

uint16_t Processor::readBXW()
{
	return readW(readBX());
}

uint16_t Processor::readBWY()
{
	return readW(readByte()) + regs.Y;
}

void Processor::updateNZ()
{
	updateNZ(regs.A);
}

void Processor::updateNZ(uint16_t value)
{
	if (value & (getFlag(FlagM) ? 128 : 32768)) {
		setFlag(Sign);
	} else {
		clearFlag(Sign);
	}

	if (value == 0) {
		setFlag(Zero);
	} else {
		clearFlag(Zero);
	}
}

void Processor::updateNZX(uint16_t value)
{
	if (value & (getFlag(FlagX) ? 128 : 32768)) {
		setFlag(Sign);
	} else {
		clearFlag(Sign);
	}

	if (value == 0) {
		setFlag(Zero);
	} else {
		clearFlag(Zero);
	}
}

void Processor::push1(uint8_t value)
{
	if (getFlag(FlagE)) {
		regs.SP = ((regs.SP - 1) & 0xff) | (regs.SP & 0xff00);
	} else {
		--regs.SP;
	}

	writeMemory(regs.SP, value);
}

void Processor::push1r(uint8_t value)
{
	writeMemory(--regs.R, value);
}

void Processor::push2(uint16_t value)
{
	push1(value >> 8);
	push1(value);
}

void Processor::push2r(uint16_t value)
{
	push1r(value >> 8);
	push1r(value);
}

void Processor::pushM(uint16_t value)
{
	if (getFlag(FlagM)) {
		push1(value);
	} else {
		push2(value);
	}
}

void Processor::pushX(uint16_t value)
{
	if (getFlag(FlagX)) {
		push1(value);
	} else {
		push2(value);
	}
}

void Processor::pushMr(uint16_t value)
{
	if (getFlag(FlagM)) {
		push1r(value);
	} else {
		push2r(value);
	}
}

void Processor::pushXr(uint16_t value)
{
	if (getFlag(FlagX)) {
		push1r(value);
	} else {
		push2r(value);
	}
}

uint8_t Processor::pop1()
{
	uint8_t i = readMemory(regs.SP);
	if (getFlag(FlagE)) {
		regs.SP = ((regs.SP + 1) & 0xff) | (regs.SP & 0xff00);
	} else {
		++regs.SP;
	}

	return i;
}

uint8_t Processor::pop1r()
{
	return readMemory(regs.R++);
}

uint16_t Processor::pop2()
{
	uint16_t i = pop1();
	i |= pop1() << 8;
	return i;
}

uint16_t Processor::pop2r()
{
	uint16_t i = pop1r();
	i |= pop1r() << 8;
	return i;
}

uint16_t Processor::popM()
{
	if (getFlag(FlagM)) {
		return pop1();
	}

	return pop2();	
}

uint16_t Processor::popX()
{
	if (getFlag(FlagX)) {
		return pop1();
	}

	return pop2();
}

uint16_t Processor::popMr()
{
	if (getFlag(FlagM)) {
		return pop1r();
	}

	return pop2r();
}

uint16_t Processor::popXr()
{
	if (getFlag(FlagX)) {
		return pop1r();
	}

	return pop2r();
}

void Processor::i_brc(bool condition)
{
	int8_t i = readByte();
	if (condition) {
		std::cout << "Branch to " << +i << std::endl;
		regs.PC += i;
	} else {
		std::cout << "No branch to " << +i << std::endl;
	}
}

void Processor::i_cmp(uint16_t x, uint16_t y)
{
	if (x >= y) {
		setFlag(Carry);
	} else {
		clearFlag(Carry);
	}

	if (x == y) {
		setFlag(Zero);
	} else {
		clearFlag(Zero);
	}

	x -= y;
	if (x & (getFlag(FlagM) ? 128 : 32768)) {
		setFlag(Sign);
	} else {
		clearFlag(Sign);
	}
}

void Processor::i_inc(uint16_t address)
{
	uint16_t i = readM(address);
	i = (i + 1) & (getFlag(FlagM) ? 255 : 65535);
	writeM(address, i);
	updateNZ(i);
}

void Processor::i_or(uint16_t value)
{
	regs.A |= value;
	updateNZ();
}

void Processor::processMMU(uint8_t opcode)
{
	std::cout << std::hex << "Got MMU opcode: " << +opcode << std::dec << std::endl;

	switch (opcode) {
	case 0x00:
		if (mmu.redbusAddress != (regs.A & 0xff)) {
			if (rbCache != nullptr) {
				rbTimeout = true;
			}

			mmu.redbusAddress = (regs.A & 0xff);
		}
		std::cout << "Redbus window mapped to device " << +mmu.redbusAddress << std::endl;
		break;
	case 0x01:
		mmu.redbusWindow = regs.A;
		std::cout << "Redbus window set to " << +mmu.redbusWindow << std::endl;
		break;
	case 0x02:
		mmu.redbusEnabled = true;
		std::cout << "Redbus enabled" << std::endl;
		break;
	case 0x03:
		mmu.externalWindow = regs.A;
		std::cout << "External redbus window set to " << +mmu.externalWindow << std::endl;
		break;
	case 0x04:
		mmu.externalWindowEnabled = true;
		std::cout << "Redbus external window enabled" << std::endl;
		break;
	case 0x06:
		porAddress = regs.A;
		break;
	case 0x82:
		mmu.redbusEnabled = false;
		std::cout << "Redbus disabled" << std::endl;
		break;
	case 0x84:
		mmu.externalWindowEnabled = false;
		std::cout << "Redbus external window disabled" << std::endl;
		break;
	default:
		std::cout << "Unknown MMU opcode: " << std::hex << +opcode << std::dec << std::endl;
		isRunning = false;
		break;
	}
}

void Processor::processInstruction()
{
	uint8_t opcode = readMemory(regs.PC++);
	std::cout << std::hex << "Got opcode: " << +opcode << std::dec << " (" << +opcode << ")" << std::endl;

	switch (opcode) {
	case 0x01:
		i_or(readM(readBXW())); break;
	case 0x02:
		regs.PC = readW(regs.I);
		regs.I += 2; break;
	case 0x03:
		i_or(readM(readBS())); break;
	case 0x05:
		i_or(readM(readByte())); break;
	case 0x07:
		i_or(readM(readBR())); break;
	case 0x18:
		clearFlag(Carry); break;
	case 0x1a:
		regs.X = (regs.X + 1) & (getFlag(FlagM) ? 255 : 65535);
		updateNZ(regs.A); break;
	case 0x22:
		push2r(regs.I);
		regs.I = regs.PC + 2;
		regs.PC = readW(regs.PC); break;
	case 0x2b:
		regs.I = pop2r();
		updateNZX(regs.I); break;
	case 0x30:
		i_brc(getFlag(Sign)); break;
	case 0x38:
		setFlag(Carry); break;
	case 0x42:
		if (getFlag(FlagM)) {
			regs.A = readMemory(regs.I++);
		} else {
			regs.A = readW(regs.I);
			regs.I += 2;
		}
		break;
	case 0x48:
		pushM(regs.A); break;
	case 0x4b:
		pushMr(regs.A); break;
	case 0x4c:
		regs.PC = readW(); std::cout << "GOTO " << regs.PC << std::endl; break;
	case 0x5c:
		regs.I = regs.X;
		updateNZX(regs.X); break;
	case 0x64:
		writeM(readByte(), 0); break;
	case 0x68:
		regs.A = popM();
		updateNZ(); break;
	case 0x6b:
		regs.A = popMr();
		updateNZ(regs.A); break;
	case 0x85:
		writeM(readByte(), regs.A); break;
	case 0x88:
		regs.Y = (regs.Y - 1) & (getFlag(FlagX) ? 255 : 65535);
		updateNZ(regs.Y); break;
	case 0x8d:
		writeM(readW(), regs.A); break;
	case 0x8f:
		regs.D = regs.B = 0; break;
	case 0x92:
		writeM(readBW(), regs.A); break;
	case 0x95:
		writeM(readBX(), regs.A); break;
	case 0xa0:
		regs.Y = readX();
		updateNZ(regs.Y); break;
	case 0xa1:
		regs.A = readM(readBXW());
		updateNZ(); break;
	case 0xa2:
		regs.X = readX();
		updateNZ(regs.X); break;
	case 0xa3:
		regs.A = readM(readBS());
		updateNZ(); break;
	case 0xa5:
		regs.A = readM(readByte());
		updateNZ(); break;
	case 0xa9:
		regs.A = readM();
		updateNZ(); break;
	case 0xaa:
		regs.X = regs.A;
		if (getFlag(FlagX)) {
			regs.X &= 0xff;
		}
		updateNZX(regs.X); break;
	case 0xad:
		regs.A = readM(readW());
		updateNZ(); break;
	case 0xb5:
		regs.A = readM(readBX());
		updateNZ(); break;
	case 0xc2:
		resetFlags(readByte()); break;
	case 0xc3:
		i_cmp(regs.A, readM(readBS())); break;
	case 0xcb:
		std::cout << "Processing WAI" << std::endl;
		waiTimeout = true; break;
	case 0xcd:
		i_cmp(regs.A, readM(readW())); break;
	case 0xd0:
		i_brc(!getFlag(Zero)); break;
	case 0xda:
		pushX(regs.X); break;
	case 0xdc:
		regs.X = regs.I;
		if (getFlag(FlagX)) {
			regs.X &= 0xff;
		}
		updateNZX(regs.X); break;
	case 0xe2:
		setFlags(readByte()); break;
	case 0xe6:
		i_inc(readByte()); break;
	case 0xef:
		processMMU(readByte()); break;
	case 0xf0:
		i_brc(getFlag(Zero)); break;
	case 0xf4:
		push2(readW()); break;
	case 0xfa:
		regs.X = popX();
		updateNZX(regs.X); break;
	case 0xfb:
		if (getFlag(FlagE) == getFlag(Carry)) {
			break;
		}

		if (getFlag(FlagE)) {
			clearFlag(FlagE);
			setFlag(Carry);
		} else {
			setFlag(FlagE);
			clearFlag(Carry);
			if (!getFlag(FlagM)) {
				regs.B = regs.A >> 8;
			}
			setFlag(FlagM);
			setFlag(FlagX);
			regs.A &= 0xff;
			regs.Y &= 0xff;
			regs.X &= 0xff;
		}

		break;
	default:
		std::cout << "Unknown opcode: " << std::hex << +opcode << " at " << regs.PC-1 << std::dec << std::endl;
		isRunning = false;
		break;
	}
}

void Processor::loadBootImage()
{
	try {
		auto bootImage = loadFile(bootImagePath);
		for (unsigned i = 0; i < bootImageSize && i < bootImage.size(); ++i) {
			memory[bootImageOffset + i] = bootImage[i];
		}
	} catch (std::runtime_error & e) {
		std::cout << e.what() << std::endl;
	}
}