/*
* Copyright (C) 2009, HustMoon Studio
*
* 文件名称：myfunc.c
* 摘	要：认证相关算法及方法
* 作	者：HustMoon@BYHH
*/
#include "myfunc.h"
#include "md5.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#ifndef SIOCGIFHWADDR	/* BSD、MacOS */
#include <net/if_dl.h>
#include <ifaddrs.h>
#endif

const u_char STANDARD_ADDR[] = {0x01,0x80,0xC2,0x00,0x00,0x03};
const u_char RUIJIE_ADDR[] = {0x01,0xD0,0xF8,0x00,0x00,0x03};

static u_int32_t readSize = 0, checkSize = 0;	/* 读取大小、校验大小 */
static u_int32_t echoKey = 0, echoNo = 0;	/* Echo阶段所需 */
u_char *fillBuf = NULL;	/* 填充包地址 */
unsigned fillSize = 0;	/* 填充包大小 */

extern char password[];
extern char nic[];
extern char dataFile[];
extern u_int32_t ip, mask, gateway, dns;
extern u_char localMAC[], destMAC[];
extern unsigned startMode, dhcpMode;

static void showFileError();	/* 获取网络地址 */
static int getAddress();	/* 获取网络地址 */
static u_char encode(u_char base);	/* 锐捷算法，颠倒一个字节的8位 */
static void checkSum(u_char *buf);	/* 锐捷算法，计算两个字节的检验值 */
static int setProperty(u_char type, const u_char *value, int length);	/* 设置指定属性 */
static int readPacket(int type);	/* 读取数据 */
static int V2Check(const u_char *md5Seed);	/* V2校验算法 */

char *formatIP(u_int32_t ip)
{
	static char tmp[16];
	u_char *p = (u_char *)(&ip);
	sprintf(tmp, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
	return tmp;
}

char *formatHex(const void *buf, int length)
{
	static char hex[385];
	u_char *p = (u_char *)buf;
	int i;
	if (length > 128)
		length = 128;
	for (i=0; i<length; i++)
		sprintf(hex+3*i,"%02x:", p[i]);
	hex[3*length-1] = '\0';
	return hex;
}

static void showFileError()
{
	printf("!! 所选文件%s无效，改用内置数据认证。\n", dataFile);
	dataFile[0] = '\0';
}

void newBuffer()
{
	u_char buf[16];
	FILE *fp = NULL;
	if ((fp=fopen(dataFile, "rb")) == NULL)
		goto fileError;
	if (fread(buf, 16, 1, fp) < 1)
	{
		fclose(fp);
		goto fileError;
	}
	readSize = (*(u_int32_t *)buf ^ *(u_int32_t *)(buf + 8)) + 16;
	checkSize = (*(u_int32_t *)buf ^ *(u_int32_t *)(buf + 12)) + 16;
	fseek(fp, 0, SEEK_END);
	fillSize = ftell(fp);
	fclose(fp);
	if (fillSize < readSize)
		goto fileError;
	fillSize = (fillSize - readSize) / 2 + 0x17;
	if (checkSize<readSize || fillSize<0x80)
		goto fileError;
	fillBuf = (u_char *)malloc(fillSize);
	return;

fileError:
	if (dataFile[0] != '\0')
		showFileError();
	fillSize = 0x80;
	fillBuf = (u_char *)malloc(fillSize);
}

static int getAddress()
{
	struct ifreq ifr;
#ifndef SIOCGIFHWADDR	/* BSD、MacOS */
	struct ifaddrs *ifap, *p = NULL;
	struct sockaddr_dl *sdl;
#endif
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
	{
		printf("!! 创建套接字失败!\n");
		return -1;
	}
	strcpy(ifr.ifr_name, nic);

#ifdef SIOCGIFHWADDR
	if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0)
		goto getMACError;
	memcpy(localMAC, ifr.ifr_hwaddr.sa_data, 6);
#else
	if (getifaddrs(&ifap) == 0)
	{
		for (p=ifap; p; p=p->ifa_next)
		{
			if (p->ifa_name && strcmp(p->ifa_name, nic)==0)
			{
				sdl = (struct sockaddr_dl *)p->ifa_addr;
				memcpy(localMAC, sdl->sdl_data + sdl->sdl_nlen, 6);
				break;
			}
		}
		freeifaddrs(ifap);
	}
	if (p == NULL)
		goto getMACError;
#endif

	if (startMode == 0)
		memcpy(destMAC, STANDARD_ADDR, 6);
	else if (startMode == 1)
		memcpy(destMAC, RUIJIE_ADDR, 6);

	if (dhcpMode!=0 || ip==0)
	{
		if (ioctl(sock, SIOCGIFADDR, &ifr) < 0)
			printf("!! 在网卡%s上获取IP失败!\n", nic);
		else
			ip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
	}

	if (ioctl(sock, SIOCGIFNETMASK, &ifr) < 0)
		printf("!! 在网卡%s上获取子网掩码失败!\n", nic);
	else
		mask = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr;
	close(sock);

	if (startMode%3==2 && gateway==0)	/* 赛尔且未填写网关地址 */
		gateway = (ip & 0x00FFFFFF) | 0x02000000;	/* 据说赛尔的网关是ip前三字节，后一字节是2 */

	printf("** 本机MAC:\t%s\n", formatHex(localMAC, 6));
	printf("** 使用IP:\t%s\n", formatIP(ip));
	printf("** 子网掩码:\t%s\n", formatIP(mask));
	return 0;

getMACError:
	close(sock);
	printf("!! 在网卡%s上获取MAC失败!\n", nic);
	return -1;
}

static u_char encode(u_char base)	/* 算法，将一个字节的8位颠倒并取反 */
{
	u_char result = 0;
	int i;
	for (i=0; i<8; i++)
	{
		result <<= 1;
		result |= base&0x01;
		base >>= 1;
	}
	return ~result;
}

static void checkSum(u_char *buf)	/* 算法，计算两个字节的checksum */
{
	u_char table[] =
	{
		0x00,0x00,0x21,0x10,0x42,0x20,0x63,0x30,0x84,0x40,0xA5,0x50,0xC6,0x60,0xE7,0x70,
		0x08,0x81,0x29,0x91,0x4A,0xA1,0x6B,0xB1,0x8C,0xC1,0xAD,0xD1,0xCE,0xE1,0xEF,0xF1,
		0x31,0x12,0x10,0x02,0x73,0x32,0x52,0x22,0xB5,0x52,0x94,0x42,0xF7,0x72,0xD6,0x62,
		0x39,0x93,0x18,0x83,0x7B,0xB3,0x5A,0xA3,0xBD,0xD3,0x9C,0xC3,0xFF,0xF3,0xDE,0xE3,
		0x62,0x24,0x43,0x34,0x20,0x04,0x01,0x14,0xE6,0x64,0xC7,0x74,0xA4,0x44,0x85,0x54,
		0x6A,0xA5,0x4B,0xB5,0x28,0x85,0x09,0x95,0xEE,0xE5,0xCF,0xF5,0xAC,0xC5,0x8D,0xD5,
		0x53,0x36,0x72,0x26,0x11,0x16,0x30,0x06,0xD7,0x76,0xF6,0x66,0x95,0x56,0xB4,0x46,
		0x5B,0xB7,0x7A,0xA7,0x19,0x97,0x38,0x87,0xDF,0xF7,0xFE,0xE7,0x9D,0xD7,0xBC,0xC7,
		0xC4,0x48,0xE5,0x58,0x86,0x68,0xA7,0x78,0x40,0x08,0x61,0x18,0x02,0x28,0x23,0x38,
		0xCC,0xC9,0xED,0xD9,0x8E,0xE9,0xAF,0xF9,0x48,0x89,0x69,0x99,0x0A,0xA9,0x2B,0xB9,
		0xF5,0x5A,0xD4,0x4A,0xB7,0x7A,0x96,0x6A,0x71,0x1A,0x50,0x0A,0x33,0x3A,0x12,0x2A,
		0xFD,0xDB,0xDC,0xCB,0xBF,0xFB,0x9E,0xEB,0x79,0x9B,0x58,0x8B,0x3B,0xBB,0x1A,0xAB,
		0xA6,0x6C,0x87,0x7C,0xE4,0x4C,0xC5,0x5C,0x22,0x2C,0x03,0x3C,0x60,0x0C,0x41,0x1C,
		0xAE,0xED,0x8F,0xFD,0xEC,0xCD,0xCD,0xDD,0x2A,0xAD,0x0B,0xBD,0x68,0x8D,0x49,0x9D,
		0x97,0x7E,0xB6,0x6E,0xD5,0x5E,0xF4,0x4E,0x13,0x3E,0x32,0x2E,0x51,0x1E,0x70,0x0E,
		0x9F,0xFF,0xBE,0xEF,0xDD,0xDF,0xFC,0xCF,0x1B,0xBF,0x3A,0xAF,0x59,0x9F,0x78,0x8F,
		0x88,0x91,0xA9,0x81,0xCA,0xB1,0xEB,0xA1,0x0C,0xD1,0x2D,0xC1,0x4E,0xF1,0x6F,0xE1,
		0x80,0x10,0xA1,0x00,0xC2,0x30,0xE3,0x20,0x04,0x50,0x25,0x40,0x46,0x70,0x67,0x60,
		0xB9,0x83,0x98,0x93,0xFB,0xA3,0xDA,0xB3,0x3D,0xC3,0x1C,0xD3,0x7F,0xE3,0x5E,0xF3,
		0xB1,0x02,0x90,0x12,0xF3,0x22,0xD2,0x32,0x35,0x42,0x14,0x52,0x77,0x62,0x56,0x72,
		0xEA,0xB5,0xCB,0xA5,0xA8,0x95,0x89,0x85,0x6E,0xF5,0x4F,0xE5,0x2C,0xD5,0x0D,0xC5,
		0xE2,0x34,0xC3,0x24,0xA0,0x14,0x81,0x04,0x66,0x74,0x47,0x64,0x24,0x54,0x05,0x44,
		0xDB,0xA7,0xFA,0xB7,0x99,0x87,0xB8,0x97,0x5F,0xE7,0x7E,0xF7,0x1D,0xC7,0x3C,0xD7,
		0xD3,0x26,0xF2,0x36,0x91,0x06,0xB0,0x16,0x57,0x66,0x76,0x76,0x15,0x46,0x34,0x56,
		0x4C,0xD9,0x6D,0xC9,0x0E,0xF9,0x2F,0xE9,0xC8,0x99,0xE9,0x89,0x8A,0xB9,0xAB,0xA9,
		0x44,0x58,0x65,0x48,0x06,0x78,0x27,0x68,0xC0,0x18,0xE1,0x08,0x82,0x38,0xA3,0x28,
		0x7D,0xCB,0x5C,0xDB,0x3F,0xEB,0x1E,0xFB,0xF9,0x8B,0xD8,0x9B,0xBB,0xAB,0x9A,0xBB,
		0x75,0x4A,0x54,0x5A,0x37,0x6A,0x16,0x7A,0xF1,0x0A,0xD0,0x1A,0xB3,0x2A,0x92,0x3A,
		0x2E,0xFD,0x0F,0xED,0x6C,0xDD,0x4D,0xCD,0xAA,0xBD,0x8B,0xAD,0xE8,0x9D,0xC9,0x8D,
		0x26,0x7C,0x07,0x6C,0x64,0x5C,0x45,0x4C,0xA2,0x3C,0x83,0x2C,0xE0,0x1C,0xC1,0x0C,
		0x1F,0xEF,0x3E,0xFF,0x5D,0xCF,0x7C,0xDF,0x9B,0xAF,0xBA,0xBF,0xD9,0x8F,0xF8,0x9F,
		0x17,0x6E,0x36,0x7E,0x55,0x4E,0x74,0x5E,0x93,0x2E,0xB2,0x3E,0xD1,0x0E,0xF0,0x1E
	};
	u_char *checkSum = buf + 0x15;
	int i, index;
	for (i=0; i<0x15; i++)
	{
		index = checkSum[0] ^ buf[i];
		checkSum[0] = checkSum[1] ^ table[index*2+1];
		checkSum[1] = table[index*2];
	}
	for (i=0; i<0x17; i++)
		buf[i] = encode(buf[i]);
}

int fillHeader()
{
	if (getAddress() == -1)
		return -1;
	memset(fillBuf, 0, fillSize);
	fillBuf[0x02] = 0x13;
	fillBuf[0x03] = 0x11;
	if (dhcpMode != 0)
		fillBuf[0x04] = 0x01;		/* DHCP位，使用1、不使用0 */
	memcpy(fillBuf+0x05, &ip, 4);
	memcpy(fillBuf+0x09, &mask, 4);
	memcpy(fillBuf+0x0D, &gateway, 4);
	memcpy(fillBuf+0x11, &dns, 4);
	checkSum(fillBuf);
	return 0;
}

static int setProperty(u_char type, const u_char *value, int length)
{
	u_char *p = fillBuf+0x46, *end = fillBuf+fillSize-length-8;	/* 形如1a 28 00 00 13 11 17 22，至少8个字节 */
	while (p <= end)
	{
		if (*p == 0x1a)	/* 有些老版本没有前两个字节，包括xrgsu */
			p += 2;
		if (p[4] == type)
		{
			memcpy(p+4+p[5]-length, value, length);
			return 0;
		}
		p += p[5] + 4;
	}
	return -1;
}

static int readPacket(int type)
{
	u_char dhcp[] = {0x00};
	FILE *fp = fopen(dataFile, "rb");
	if (fp == NULL)
		goto fileError;
	type %= 2;	/* 偶数读Start包，奇数读Md5包 */
	fseek(fp, readSize+(fillSize-0x17)*type, SEEK_SET);
	if (fread(fillBuf+0x17, fillSize-0x17, 1, fp) < 1)	/* 前0x17字节是地址及校验值 */
	{
		fclose(fp);
		goto fileError;
	}
	fclose(fp);
	if (dhcpMode == 1)	/* 二次认证第一次 */
		dhcp[0] = 0x01;
	setProperty(0x18, dhcp, 1);
	setProperty(0x2D, localMAC, 6);
	return 0;

fileError:
	showFileError();
	return -1;
}

void fillStartPacket()
{
	if (dataFile[0] == '\0')	/* 不使用数据包？ */
	{
		u_char packet[] =
		{
			0x00,0x00,0x13,0x11,0x38,0x30,0x32,0x31,0x78,0x2e,0x65,0x78,0x65,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x13,0x11,0x00,0x28,0x1a,
			0x28,0x00,0x00,0x13,0x11,0x17,0x22,0x91,0x66,0x64,0x93,0x67,0x60,0x65,0x62,0x62,
			0x94,0x61,0x69,0x67,0x63,0x91,0x93,0x92,0x68,0x66,0x93,0x91,0x66,0x95,0x65,0xaa,
			0xdc,0x64,0x98,0x96,0x6a,0x9d,0x66,0x00,0x00,0x13,0x11,0x18,0x06,0x02,0x00,0x00
		};
		memcpy(fillBuf+0x17, packet, sizeof(packet));
		if (dhcpMode == 1)
			fillBuf[0x77] = 0x01;
		else
			fillBuf[0x77] = 0x00;
	}
	else if (readPacket(0) == -1)	/* 读取数据失败就用默认的填充 */
		fillStartPacket();
}

void fillMd5Packet(const u_char *md5Seed)
{
	if (dataFile[0] == '\0')	/* 不使用数据包？ */
	{
		/* xrgsu的Md5包与Start包只有一个字节的差异，若以其他版本为基础，可进一步区别对待 */
		fillStartPacket();
		fillBuf[0x3F] = 0x04;
	}
	else if (readPacket(1)==-1 || V2Check(md5Seed)==-1)
		fillMd5Packet(md5Seed);
	echoNo = 0x0000102B;	/* 初始化echoNo */
}

static int V2Check(const u_char *md5Seed)	/* V2算法 */
{
	FILE *fp = NULL;
	u_char *ruijie = NULL;
	int i, j;
	u_char table[144], *md5Dig;
	char md5[33];
	printf("** 客户端版本:%d.%d 适用:%s 类型:%d\n", fillBuf[0x3B], fillBuf[0x3C], fillBuf[0x3D]?"Linux":"Windows", fillBuf[0x3E]);
	if (readSize < 0x410)	/* 目前V3算法没有攻破，V2算法几乎废掉，我们可能只需要抓包部分 */
	{
		printf("!! 数据文件不含校验信息。\n");
		return 0;
	}
	if ((fp=fopen(dataFile, "rb")) == NULL)
		goto fileError;
	ruijie = (u_char *)malloc(checkSize);
	if (fread(ruijie, readSize, 1, fp) < 1)
	{
		fclose(fp);
		free(ruijie);
		goto fileError;
	}
	fclose(fp);
	for (i=16; i<readSize; i+=16)	/* 还原数据，这是从Windows版的MentoHUST遗留下来的，如果V3被攻破，倒是可以重新定义一个格式 */
	{
		for (j=0; j<16; j++)
			ruijie[i+j] ^= ruijie[j];
	}
	memset(ruijie+readSize, 0, checkSize-readSize);
	j = (checkSize - 16) / 8;	/* 数据量现在清楚了，下面用Soar的算法开始计算相应MD5值 */
	for (i=0; i<8; i++)
	{
		memcpy(ruijie + j * i, md5Seed, 16);
		md5Dig = ComputeHash(ruijie + j * i, j + 16);
		table[18*i] = md5Seed[2*i];
		memcpy(table+18*i+1, md5Dig, 16);
		table[18*i+17] = md5Seed[2*i+1];
	}
	free(ruijie);
	md5Dig = ComputeHash(table, 144);
	for (i=0; i<16; i++)
		sprintf(md5+2*i,"%02x", md5Dig[i]);
	printf("** MD5校验值:\t%s\n", md5);
	setProperty(0x17, (u_char *)md5, 32);
	return 0;

fileError:
	showFileError();
	return -1;
}

void fillEchoPacket(u_char *echoBuf)
{
	int i;
	u_char bt1[4], bt2[4];
	*(u_int32_t *)bt1 = htonl(echoKey + echoNo);
	*(u_int32_t *)bt2 = htonl(echoNo);
	echoNo++;
	for (i=0; i<4; i++)
	{
		echoBuf[0x18+i] = encode(bt1[i]);
		echoBuf[0x22+i] = encode(bt2[i]);
	}
}

void getEchoKey(const u_char *capBuf)
{
	int i, offset = 0x1c+capBuf[0x1b]+0x69+24;	/* 通过比较了大量抓包，通用的提取点就是这样的 */
	u_char *base;
	echoKey = ntohl(*(u_int32_t *)(capBuf+offset));
	base = (u_char *)(&echoKey);
	for (i=0; i<4; i++)
		base[i] = encode(base[i]);
}

u_char *checkPass(u_char id, const u_char *md5Seed, int seedLen)
{
	u_char md5Src[80];
	int md5Len = strlen(password);
	md5Src[0] = id;
	memcpy(md5Src+1, password, md5Len);
	md5Len++;
	if (startMode % 3 == 2)	/* 赛尔？ */
	{
		memcpy(md5Src+md5Len, "xxghlmxhzb", 10);
		md5Len += 10;
	}
	memcpy(md5Src+md5Len, md5Seed, seedLen);
	md5Len += seedLen;
	return ComputeHash(md5Src, md5Len);
}

void fillCernetAddr(u_char *buf)
{
	memcpy(buf+0x18, &ip, 4);
	memcpy(buf+0x1C, &mask, 4);
	memcpy(buf+0x20, &gateway, 4);
	memset(buf+0x24, 0, 4);	/* memcpy(buf+0x24, &dns, 4); */
}