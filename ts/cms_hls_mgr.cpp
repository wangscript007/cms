/*
The MIT License (MIT)

Copyright (c) 2017- cms(hsc)

Author: ���û������/kisslovecsh@foxmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include <ts/cms_hls_mgr.h>
#include <ts/cms_ts_chunk.h>
#include <log/cms_log.h>
#include <common/cms_utility.h>
#include <flvPool/cms_flv_var.h>
#include <flvPool/cms_flv_pool.h>
#include <app/cms_app_info.h>
#include <ts/cms_ts_callback.h>
#include <time.h>
#include <assert.h>

//TEST
std::map<unsigned long, unsigned long> gmapSendTakeTime;
int64 gSendTakeTimeTT;
CLock gSendTakeTime;
//TEST end

uint64 guid[APP_ALL_MODULE_THREAD_NUM];
CLock guidLock[APP_ALL_MODULE_THREAD_NUM];

uint64 getUid(uint32 i)
{
	uint64 uid;
	guidLock[i].Lock();
	uid = guid[i];
	guid[i] += APP_ALL_MODULE_THREAD_NUM;
	guidLock[i].Unlock();
	return uid;
}

SSlice *newSSlice()
{
	SSlice *ss = new SSlice();
	ss->mionly = 0;
	ss->msliceRange = 0;  //��Ƭʱ��
	ss->msliceLen = 0;    //��Ƭ��С
	ss->msliceIndex = 0;  //��Ƭ���
	ss->msliceStart = 0;  //��Ƭ��ʼʱ���
	atomicInc(ss);
	return ss;
}

void atomicInc(SSlice *s)
{
	__sync_add_and_fetch(&s->mionly, 1);//�����ݳ�ʱ����û��ʹ��ʱ��ɾ��
}

void atomicDec(SSlice *s)
{
	if (s && __sync_sub_and_fetch(&s->mionly, 1) == 0)//�����ݳ�ʱ����û��ʹ��ʱ��ɾ��
	{
		TsChunkArray *tca = NULL;
		//TsChunk *tc = NULL;
		std::vector<TsChunkArray *>::iterator itTca = s->marray.begin();
		for (; itTca != s->marray.end();)
		{
			tca = *itTca;
			freeTsChunkArray(tca);
			delete tca;
			itTca = s->marray.erase(itTca);
		}
	}
}

CMission::CMission(HASH &hash, uint32 hashIdx, std::string url,
	int tsDuration, int tsNum, int tsSaveNum)
{
	mevLoop = NULL;
	mtimerEtp.idx = 0;
	mtimerEtp.uid = 0;

	mhash = hash;
	mhashIdx = hashIdx;
	murl = url;

	muid = ::getUid(hashIdx%APP_ALL_MODULE_THREAD_NUM);

	mtsDuration = tsDuration;
	mtsNum = tsNum;
	mtsSaveNum = tsSaveNum;
	msliceCount = 0;    //��Ƭ����
	msliceIndx = 0;     //��ǰ��Ƭ�����

	if (mtsSaveNum < mtsNum)
	{
		logs->warn("[CMission::CMission] %s ts save num %d is less than ts num %d", mtsSaveNum, mtsNum);
		mtsSaveNum = mtsNum + 2;
	}

	for (int i = 0; i < mtsSaveNum/*+mtsNum*/ + 1; i++)
	{
		msliceList.push_back(NULL);
	}
	msliceList[0] = newSSlice();

	misStop = false;

	mreadIndex = -1;		//��ȡ��֡�����

	mreadFAIndex = -1;	//��ȡ����Ƶ��֡�����
	mreadFVIndex = -1;	//��ȡ����Ƶ��֡�����

	mFAFlag = false;	//�Ƿ������֡��Ƶ
	mFVFlag = false;	//�Ƿ������֡��Ƶ(SPS/PPS)
	mbTime = 0;			//���һ����Ƭ������ʱ��
	mMux = new CSMux(); //ת����
	mlastTca = NULL;
	mullTransUid = 0;

	mdurationtt = new CDurationTimestamp();
}

CMission::~CMission()
{
	if (mMux)
	{
		delete mMux;
	}
	if (mdurationtt)
	{
		delete mdurationtt;
	}
	std::vector<SSlice *>::iterator it = msliceList.begin();
	for (; it != msliceList.end();)
	{
		atomicDec(*it);
		it = msliceList.erase(it);
	}
}

int CMission::doFirstVideoAudio(bool isVideo)
{
	int ret = CMS_OK;
	Slice *s = NULL;
	if (CFlvPool::instance()->readFirstVideoAudioSlice(mhashIdx, mhash, &s, isVideo) == FlvPoolCodeError)
	{
		return ret;
	}
	assert(s != NULL);
	byte *outBuf = NULL;
	int  outLen = 0;
	if (isVideo)
	{
		logs->debug(">>>>>[CMission::doFirstVideoAudio] %s doFirstVideoAudio parse first video",
			murl.c_str());
		mreadFVIndex = s->mllIndex;
		mMux->parseNAL((byte *)s->mData, s->miDataLen, &outBuf, outLen);
		mFVFlag = true;
	}
	else
	{
		logs->debug(">>>>>[CMission::doFirstVideoAudio] %s doFirstVideoAudio parse first audio",
			murl.c_str());
		mreadFAIndex = s->mllIndex;
		mMux->parseAAC((byte *)s->mData, s->miDataLen, &outBuf, outLen);
		mFAFlag = true;
	}
	if (outBuf)
	{
		delete[] outBuf;
	}
	atomicDec(s);
	return ret;
}

int  CMission::doit()
{
	int ret = 0;
	Slice *s = NULL;
	int flvPoolCode;
	uint32 uiTimestamp = 0;
	int	sliceNum = 0;
	bool isVideo = false;
	bool isAudio = false;
	bool isTransPlay = false;
	bool isExist = false;
	bool isPublishTask = false;
	bool isTaskRestart = false;
	bool isMetaDataChanged = false;
	bool isFirstVideoAudioChanged = false;
	int64 llMetaDataIdx = -1;
	do
	{
		isTransPlay = false;
		isExist = false;
		isPublishTask = false;
		isTaskRestart = false;
		isMetaDataChanged = false;
		isFirstVideoAudioChanged = false;

		sliceNum = 0;
		flvPoolCode = CFlvPool::instance()->readSlice(mhashIdx, mhash, mreadIndex, &s, sliceNum, isTransPlay,
			llMetaDataIdx, mreadFVIndex, mreadFAIndex, isExist, isTaskRestart, isPublishTask, isMetaDataChanged, isFirstVideoAudioChanged, mullTransUid);
		CAutoSlice autoSlice(s);
		if (!isExist)
		{
			ret = 2;
			break;
		}

		if (flvPoolCode == FlvPoolCodeError)
		{
			//logs->error("*** [CMission::doit] %s task is missing ***",
			//	murl.c_str());
			//ret = -1;
			break;
		}
		else if (flvPoolCode == FlvPoolCodeOK)
		{
			//��֡�ı�
			if (isFirstVideoAudioChanged)
			{
				if (CFlvPool::instance()->isFirstVideoChange(mhashIdx, mhash, mreadFVIndex))
				{
					if (doFirstVideoAudio(true) == CMS_ERROR)
					{
						ret = -1;
						break;
					}
				}
				if (CFlvPool::instance()->isFirstAudioChange(mhashIdx, mhash, mreadFAIndex))
				{
					if (doFirstVideoAudio(false) == CMS_ERROR)
					{
						ret = -1;
						break;
					}
				}
			}

			if (!mdurationtt->isInit())
			{
				mdurationtt->init("0.0.0.0", "flv", murl);
				mdurationtt->setResetTimestamp(false);
			}
			if (s)
			{
				isVideo = s->miDataType == DATA_TYPE_VIDEO;
				isAudio = s->miDataType == DATA_TYPE_AUDIO;

				uiTimestamp = s->muiTimestamp;
				//Ԥ��ʱ�����С�����
				uiTimestamp = mdurationtt->keepTimestampIncrease(isVideo, uiTimestamp);
				//Ԥ��ʱ�����С����� ����
				if (mreadIndex + 1 != s->mllIndex && mreadIndex != -1)
				{
					logs->warn("*** [CMission::doit] %s doit drop frame,cur idx %lld,next idx %lld ***",
						murl.c_str(), mreadIndex, s->mllIndex);
				}
				mreadIndex = s->mllIndex;


				byte frameType;
				uint64 pts64;

				TsChunkArray *tca = NULL;

				if (isAudio) {
					frameType = 'A';
					mMux->packTS((byte *)s->mData, s->miDataLen, frameType, uiTimestamp, 0x01, 0x01, &mMux->getAmcc(), Apid, &tca, mlastTca, pts64);
					if (!mFAFlag)
					{
					}
				}
				else if (isVideo)
				{
					frameType = 'P';
					if (s->mData[0] == 0x17)
					{
						frameType = 'I';
					}
					mMux->packTS((byte *)s->mData, s->miDataLen, frameType, uiTimestamp, 0x01, 0x01, &mMux->getVmcc(), Vpid, &tca, mlastTca, pts64);
					if (!mFVFlag)
					{
					}
				}
				if (tca) {
					pushData(tca, frameType, pts64);
				}
				if (ret == CMS_ERROR)
				{
					ret = -1;
					break;
				}
				ret = 1;
			}
			else
			{
				break;
			}
		}
		else if (flvPoolCode == FlvPoolCodeNoData)
		{
			ret = 0;
			break;
		}
		else if (flvPoolCode == FlvPoolCodeRestart)
		{
			mreadIndex = -1;
			mreadFVIndex = -1;
			mreadFAIndex = -1;

			uint32 sendTimestamp = 0;
			if (s != NULL)
			{
				sendTimestamp = s->muiTimestamp;
			}

			mdurationtt->resetDeltaTimestamp(sendTimestamp);
			logs->debug("[CMission::doit] %s doit task is been restart",
				murl.c_str());
			ret = 0;
			break;
		}
		else
		{
			logs->error("*** [CMission::doit] %s doit unknow FlvPoolCode=%d ***",
				murl.c_str(), flvPoolCode);
			ret = -1;
			break;
		}
	} while (true);
	return ret;
}

int CMission::run(int i, struct ev_loop *loop)
{
	mevLoop = loop;
	//timer
	mtimerEtp.idx = i;
	mtimerEtp.uid = muid;
	mevTimer.data = (void *)&mtimerEtp;
	ev_timer_init(&mevTimer, ::tsTimerCallBack, 0., CMS_TS_TIMEOUT_MILSECOND); //����߳��Ƿ��ⲿֹͣ
	ev_timer_again(mevLoop, &mevTimer); //��Ҫ�ظ�
	return 0;
}

void CMission::stop()
{
	if (mevLoop)
	{
		ev_timer_stop(mevLoop, &mevTimer);
	}
}

/*����һ֡TS����
-- inbuf ��������ݻ�����
-- length ���ݴ�С
-- framtype	֡����(������Ƭ)
-- timestamp ʱ���
*/
int  CMission::pushData(TsChunkArray *tca, byte frameType, uint64 timestamp)
{
	assert(tca->msliceSize%TS_CHUNK_SIZE == 0);
	//  	logs->debug(" [CMission::doit] %s pushData enter,len=%d,frameType=%c,timestamp=%d,msliceList[msliceCount]->msliceStart=%d,mtsDuration*90000=%d",
	//  		murl.c_str(),inLen,frameType,timestamp,msliceList[msliceCount]->msliceStart,mtsDuration*90000);
	// 	logs->debug(" [CMission::doit] %s pushData (timestamp-msliceList[msliceCount]->msliceStart) > mtsDuration*90000=%s, frameType == 'I'=%s, (mFVFlag == false && frameType == 'A')=%s,frameType=%d,'I'=%d",
	// 			murl.c_str(),(timestamp-msliceList[msliceCount]->msliceStart) > mtsDuration*90000?"true":"false",
	// 			frameType == 'I'?"true":"false",(mFVFlag == false && frameType == 'A')?"true":"false",frameType,'I');
	SSlice *ss = NULL;
	if ((msliceList[msliceCount]->msliceStart != 0) && timestamp - msliceList[msliceCount]->msliceStart > (uint64)(mtsDuration * 90000) &&
		(frameType == 'I' || (mFVFlag == false && frameType == 'A')))
	{
		// 		logs->debug(" [CMission::pushData] %s pushData if enter,msliceCount+1=%d,mtsSaveNum=%d,timestamp=%llu,msliceStart=%llu,timestamp-msliceStart=%llu,msliceIndx=%lld,msliceList size=%u",
		// 			murl.c_str(),msliceCount+1,mtsSaveNum,timestamp,msliceList[msliceCount]->msliceStart,timestamp-msliceList[msliceCount]->msliceStart,msliceList[msliceCount]->msliceIndex,msliceList.size());
		mbTime = time(NULL);
		if (msliceCount + 1 > /*mtsNum+*/mtsSaveNum)
		{
			//Ҫ����׷�ӵĳ���
			if (mlastTca != NULL)
			{
				ss = msliceList[msliceCount];
				if (ss != NULL)
				{
					ss->msliceLen += mlastTca->mexSliceSize;
				}
			}

			msliceIndx++;
			ss = newSSlice();
			ss->msliceIndex = msliceIndx;
			ss->msliceStart = timestamp;
			mMux->packPSI();

			int writeLen = 0;
			TsChunkArray *ttmp = allocTsChunkArray(188 * 2);
			writeChunk(NULL, 0, ttmp, NULL, (char *)mMux->getPat(), 188, writeLen);
			writeChunk(NULL, 0, ttmp, NULL, (char *)mMux->getPmt(), 188, writeLen);

			ss->msliceLen = 188 * 2;
			ss->msliceLen += tca->msliceSize;

			ss->marray.push_back(ttmp);
			ss->marray.push_back(tca);

			msliceList.push_back(ss);

			msliceList[msliceCount]->msliceRange = (float)((timestamp - msliceList[msliceCount]->msliceStart) / 90000);

			logs->debug("[CMission::pushData] 1 %s pushData one ts succ,timestamp=%d",
				murl.c_str(), timestamp - msliceList[msliceCount - 1]->msliceStart);

			std::vector<SSlice *>::iterator it = msliceList.begin();
			ss = *it;
			msliceList.erase(it);
			atomicDec(ss);

			mlastTca = NULL;
		}
		else
		{
			//Ҫ����׷�ӵĳ���
			if (mlastTca != NULL)
			{
				ss = msliceList[msliceCount];
				if (ss != NULL)
				{
					ss->msliceLen += mlastTca->mexSliceSize;
				}
			}

			msliceCount++;
			msliceIndx++;
			ss = newSSlice();
			ss->msliceIndex = msliceIndx;
			ss->msliceStart = timestamp;
			mMux->packPSI();

			int writeLen = 0;
			TsChunkArray *ttmp = allocTsChunkArray(188 * 2);
			writeChunk(NULL, 0, ttmp, NULL, (char *)mMux->getPat(), 188, writeLen);
			writeChunk(NULL, 0, ttmp, NULL, (char *)mMux->getPmt(), 188, writeLen);

			ss->msliceLen = 188 * 2;
			ss->msliceLen += tca->msliceSize;

			ss->marray.push_back(ttmp);
			ss->marray.push_back(tca);

			msliceList[msliceCount] = ss;

			ss->msliceRange = (float)((timestamp - ss->msliceStart) / 90000);

			logs->debug("[CMission::pushData] 2 %s pushData one ts succ,timestamp=%d",
				murl.c_str(), timestamp - msliceList[msliceCount - 1]->msliceStart);

			mlastTca = NULL;
		}
	}
	else
	{
		ss = msliceList[msliceCount];
		if (ss->msliceStart == 0)
		{
			ss->msliceStart = timestamp;
		}
		ss->msliceLen += tca->msliceSize;
		//Ҫ����׷�ӵĳ���
		if (mlastTca != NULL)
		{
			ss->msliceLen += mlastTca->mexSliceSize;
		}

		ss->marray.push_back(tca);

		mlastTca = tca;
	}
	return 0;
}

/*��ȡ��ƬTS
-- Index ��Ƭ���
*/
int  CMission::getTS(int64 idx, SSlice **s)
{
	// 	logs->debug("[CMission::getTS] %s getTS idx=%lld,msliceIndx=%lld,msliceList[0]->msliceIndex=%lld",
	// 		murl.c_str(),idx,msliceIndx,msliceList[0]->msliceIndex);

	if (idx >= msliceIndx || idx < msliceList[0]->msliceIndex)
	{
		return -1;
	}
	int64 pos = idx - msliceList[0]->msliceIndex;
	*s = msliceList[pos];
	atomicInc(*s);
	return 1;
}
/*��ȡM3U8�б�
-- addr ��װ����·����addr
*/
int  CMission::getM3U8(std::string addr, std::string &outData)
{
	size_t end = murl.rfind("/");
	std::string url = murl.substr(7, end - 7);
	outData += "#EXTM3U\n";

	char szData[30] = { 0 };
	snprintf(szData, sizeof(szData), "%d", mtsDuration + 1);
	outData += "#EXT-X-TARGETDURATION:";
	outData += szData;
	outData += "\n";

	int pos = 0;
	if (msliceCount <= mtsNum)
	{
		pos = 0;
	}
	else if (msliceCount >= (mtsSaveNum/* + mtsNum*/))
	{
		pos = mtsSaveNum - mtsNum;
	}
	else
	{
		pos = msliceCount - mtsNum;
	}
	snprintf(szData, sizeof(szData), "%lld", msliceList[pos]->msliceIndex);
	outData += "#EXT-X-MEDIA-SEQUENCE:";
	outData += szData;
	outData += "\n";

	for (int64 i = msliceList[pos]->msliceIndex; i < msliceIndx; i++)
	{
		//logs->debug("\n\nm3u8 url %s ,murl %s,end %d",url.c_str(),murl.c_str(),end);
		snprintf(szData, sizeof(szData), "%0.2f", msliceList[pos]->msliceRange);
		outData += "#EXTINF:";
		outData += szData;
		outData += ",\n";
		outData += "http://";
		outData += addr;
		outData += "/";
		outData += url;
		outData += "/";
		snprintf(szData, sizeof(szData), "%lld", i);
		outData += szData;
		outData += ".ts\n";
		pos++;
	}
	return 1;
}

int64 CMission::getLastTsTime()
{
	return mbTime;
}

int64 CMission::getUid()
{
	return muid;
}

//mgr
CMissionMgr *CMissionMgr::minstance = NULL;
CMissionMgr::CMissionMgr()
{
	for (int i = 0; i < APP_ALL_MODULE_THREAD_NUM; i++)
	{
		guid[i] = i;
		misRunning[i] = false;
		mtid[i] = 0;
		mevLoop[i] = NULL;
	}
}

CMissionMgr::~CMissionMgr()
{

}

void *CMissionMgr::routinue(void *param)
{
	HlsMgrThreadParam *phmtp = (HlsMgrThreadParam *)param;
	CMissionMgr *pInstance = phmtp->pinstance;
	uint32 i = phmtp->i;
	delete phmtp;
	pInstance->thread(i);
	return NULL;
}

void CMissionMgr::thread(uint32 i)
{
	setThreadName("cms-hls-mgr");

	mtimerEtp[i].idx = i;
	mtimerEtp[i].uid = 0;
	mevTimer[i].data = (void *)&mtimerEtp[i];
	ev_timer_init(&mevTimer[i], ::tsAliveCallBack, 0., CMS_TS_TIMEOUT_MILSECOND); //����߳��Ƿ��ⲿֹͣ
	ev_timer_again(mevLoop[i], &mevTimer[i]); //��Ҫ�ظ�

	ev_run(mevLoop[i], 0);
}

bool CMissionMgr::run()
{
	if (gSendTakeTimeTT == 0)
	{
		gSendTakeTimeTT = getTimeUnix();
	}
	for (uint32 i = 0; i < APP_ALL_MODULE_THREAD_NUM; i++)
	{
		misRunning[i] = true;
		HlsMgrThreadParam *hmtp = new HlsMgrThreadParam;
		hmtp->pinstance = this;
		hmtp->i = i;
		mevLoop[i] = ev_loop_new(EVBACKEND_EPOLL | EVBACKEND_POLL | EVBACKEND_SELECT | EVFLAG_NOENV);

		if (ev_backend(mevLoop[i]) & EVBACKEND_EPOLL)
		{
			printf("gsdn epoll mode\n");
		}
		else if (ev_backend(mevLoop[i]) & EVBACKEND_POLL)
		{
			printf("gsdn poll mode\n");
		}
		else if (ev_backend(mevLoop[i]) & EVBACKEND_SELECT)
		{
			printf("gsdn select mode\n");
		}
		int res = cmsCreateThread(&mtid[i], routinue, hmtp, false);
		if (res == -1)
		{
			misRunning[i] = false;
			char date[128] = { 0 };
			getTimeStr(date);
			logs->error("%s ***** file=%s,line=%d cmsCreateThread error *****", date, __FILE__, __LINE__);
			return false;
		}
	}
	return true;
}

void CMissionMgr::stop()
{
	logs->debug("##### CMissionMgr::stop begin #####");
	for (uint32 i = 0; i < APP_ALL_MODULE_THREAD_NUM; i++)
	{
		misRunning[i] = false;
		cmsWaitForThread(mtid[i], NULL);
		mtid[i] = 0;
	}
	logs->debug("##### CMissionMgr::stop finish #####");
}

CMissionMgr *CMissionMgr::instance()
{
	if (minstance == NULL)
	{
		minstance = new CMissionMgr();
	}
	return minstance;
}

void CMissionMgr::freeInstance()
{
	if (minstance)
	{
		delete minstance;
		minstance = NULL;
	}
}

int	 CMissionMgr::create(uint32 i, HASH &hash, std::string url, int tsDuration, int tsNum, int tsSaveNum)
{
	if (g_isTestServer)
	{
		return CMS_OK;
	}

	int ret = CMS_ERROR;
	uint32 ii = i % APP_ALL_MODULE_THREAD_NUM;
	mMissionMapLock[ii].WLock();
	std::map<HASH, CMission *>::iterator it = mMissionMap[ii].find(hash);
	if (it == mMissionMap[ii].end())
	{
		CMission *cm = new CMission(hash, i, url, tsDuration, tsNum, tsSaveNum);		
		mMissionMap[ii][hash] = cm;
		mMissionUidMap[ii][cm->getUid()] = cm;
		cm->run(ii, mevLoop[ii]);
		ret = CMS_OK;
		logs->debug("### [CMissionMgr::create] thread idx=%d have hls num %d ###", i, mMissionUidMap[ii].size());
	}
	mMissionMapLock[ii].UnWLock();
	return ret;
}

void CMissionMgr::destroy(uint32 i, HASH &hash)
{
	i = i % APP_ALL_MODULE_THREAD_NUM;
	mMissionMapLock[i].WLock();
	std::map<HASH, CMission *>::iterator it = mMissionMap[i].find(hash);
	if (it != mMissionMap[i].end())
	{
		std::map<int64, CMission *>::iterator it1 = mMissionUidMap[i].find(it->second->getUid());
		if (it1 != mMissionUidMap[i].end())
		{
			mMissionUidMap[i].erase(it1);
		}

		it->second->stop();
		delete it->second;
		mMissionMap[i].erase(it);
	}
	mMissionMapLock[i].UnWLock();
}

int CMissionMgr::readM3U8(uint32 i, HASH &hash, std::string url, std::string addr, std::string &outData, int64 &tt)
{
	i = i % APP_ALL_MODULE_THREAD_NUM;
	int ret = -1;
	mMissionMapLock[i].RLock();
	std::map<HASH, CMission *>::iterator it = mMissionMap[i].find(hash);
	if (it != mMissionMap[i].end())
	{
		it->second->getM3U8(addr, outData);
		tt = it->second->getLastTsTime();
		ret = 1;
	}
	mMissionMapLock[i].UnRLock();
	return ret;
}

int CMissionMgr::readTS(uint32 i, HASH &hash, std::string url, std::string addr, SSlice **ss, int64 &tt)
{
	i = i % APP_ALL_MODULE_THREAD_NUM;
	int ret = -1;
	mMissionMapLock[i].RLock();
	std::map<HASH, CMission *>::iterator it = mMissionMap[i].find(hash);
	if (it != mMissionMap[i].end())
	{
		size_t start = url.rfind("/") + 1;
		if (start == std::string::npos)
		{

		}
		size_t end = url.find(".ts");
		if (end == std::string::npos)
		{

		}
		string strIdx = url.substr(start, end);
		int64 llIdx = _atoi64(strIdx.c_str());
		ret = it->second->getTS(llIdx, ss);
	}
	mMissionMapLock[i].UnRLock();
	return ret;
}

void CMissionMgr::release()
{

}

void CMissionMgr::tick(uint32 i, uint64 uid)
{
	unsigned long tB = getTickCount();


	i = i % APP_ALL_MODULE_THREAD_NUM;
	mMissionMapLock[i].WLock();
	std::map<int64, CMission *>::iterator it = mMissionUidMap[i].find(uid);
	if (it != mMissionUidMap[i].end())
	{
		it->second->doit();
	}
	mMissionMapLock[i].UnWLock();



	unsigned long tE = getTickCount();
	gSendTakeTime.Lock();
	int64 sendTakeTimeTT = getTimeUnix();
	bool isPrintf = false;
	if (sendTakeTimeTT - gSendTakeTimeTT > 60)
	{
		isPrintf = true;
		gSendTakeTimeTT = sendTakeTimeTT;
	}
	printTakeTime(gmapSendTakeTime, tB, tE, (char *)"CMissionMgr", isPrintf);
	gSendTakeTime.Unlock();
}

void CMissionMgr::tsAliveCallBack(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	EvTimerParam *etp = (EvTimerParam*)watcher->data;
	if (!misRunning[etp->idx])
	{
		logs->warn("@@@ [CMissionMgr::tsAliveCallBack] worker %d has been stop @@@", etp->idx);
		ev_break(EV_A_ EVBREAK_ALL);
	}
	else
	{

	}
}

void CMissionMgr::tsTimerCallBack(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	EvTimerParam *etp = (EvTimerParam*)watcher->data;
	tick(etp->idx, etp->uid);
}