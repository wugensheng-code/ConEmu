﻿
/*
Copyright (c) 2009-present Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define HIDE_USE_EXCEPTION_INFO
#define SHOWDEBUGSTR

#define SLEEP_BEFORE_VCON_CREATE 0

#include "Header.h"

#include "../common/Common.h"
#include "../common/ConEmuCheck.h"
#include "../common/MSectionSimple.h"
#include "../common/WUser.h"
#include "ConEmu.h"
#include "ConfirmDlg.h"
#include "Inside.h"
#include "Options.h"
#include "OptionsClass.h"
#include "RealConsole.h"
#include "RunQueue.h"
#include "Status.h"
#include "TabBar.h"
#include "Update.h"
#include "VConChild.h"
#include "VConGroup.h"
#include "VirtualConsole.h"

#define DEBUGSTRDRAW(s) //DEBUGSTR(s)
#define DEBUGSTRTABS(s) //DEBUGSTR(s)
#define DEBUGSTRLANG(s) //DEBUGSTR(s)
#define DEBUGSTRACTIVATE(s) //DEBUGSTR(s)
#define DEBUGSTRERR(s) //DEBUGSTR(s)
#define DEBUGSTRATTACHERR(s) DEBUGSTR(s)

static CVirtualConsole* gp_VCon[MAX_CONSOLE_COUNT] = {};

static CVirtualConsole* gp_VActive = nullptr;
//static CVirtualConsole* gp_GroupPostCloseActivate = nullptr; // Если открыты сплиты - то при закрытии одного pane - остаться в активной группе
static bool gb_CreatingActive = false, gb_SkipSyncSize = false;
static UINT gn_CreateGroupStartVConIdx = 0;
static bool gb_InCreateGroup = false;

static MSectionSimple* gpcs_VGroups = nullptr;
static CVConGroup* gp_VGroups[MAX_CONSOLE_COUNT*2] = {}; // на каждое разбиение добавляется +Parent

CVConGroup* CVConGroup::mp_GroupSplitDragging = nullptr;

#ifdef _DEBUG
MSectionSimple CRefRelease::mcs_Locks;
#endif

//CVirtualConsole* CVConGroup::mp_GrpVCon[MAX_CONSOLE_COUNT] = {};

static COORD g_LastConSize = {0,0}; // console size after last resize (in columns and lines)


/* Group Guard */

CGroupGuard::CGroupGuard(CVConGroup* apRef)
	: CRefGuard<CVConGroup>()
{
	Attach(apRef);
}

CGroupGuard::~CGroupGuard()
{
	// inherited virtual destructor
}

bool CGroupGuard::Attach(CVConGroup* apRef)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	if (mp_Ref != apRef)
	{
		CVConGroup *pOldRef = mp_Ref;

		mp_Ref = apRef;

		if (pOldRef != mp_Ref)
		{
			if (mp_Ref)
			{
				mp_Ref->AddRef();
			}

			lockGroups.Unlock();

			if (pOldRef)
			{
				pOldRef->Release();
			}
		}
	}

	return (mp_Ref != nullptr);
}





/* **************************************** */


void CVConGroup::Initialize()
{
	gpcs_VGroups = new MSectionSimple(true);
}

void CVConGroup::Deinitialize()
{
	SafeDelete(gpcs_VGroups);
}


// Вызывается при создании нового таба, без разбивки
CVConGroup* CVConGroup::CreateVConGroup()
{
	_ASSERTE(isMainThread()); // во избежание сбоев в индексах?
	CVConGroup* pGroup = new CVConGroup(nullptr);
	return pGroup;
}

CVConGroup* CVConGroup::SplitVConGroup(RConStartArgsEx::SplitType aSplitType /*eSplitHorz/eSplitVert*/, UINT anPercent10 /*= 500*/)
{
	AssertThisRet(nullptr);
	if (!(aSplitType == RConStartArgsEx::eSplitHorz || aSplitType == RConStartArgsEx::eSplitVert))
	{
		_ASSERTE(false && "Invalid split type");
		return nullptr;
	}

	if (mp_Item == nullptr)
	{
		_ASSERTE(mp_Item && "VCon was not associated");
		return nullptr;
	}

	// Разбивать можно только то, что еще не разбито ("листья")
	if (m_SplitType != RConStartArgsEx::eSplitNone)
	{
		MBoxAssert(m_SplitType == RConStartArgsEx::eSplitNone && "Can't split this pane");
		return CreateVConGroup();
	}

	// Создать две пустых панели без привязки
	_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);
	mp_Grp1 = new CVConGroup(this);
	mp_Grp2 = new CVConGroup(this);
	if (!mp_Grp1 || !mp_Grp2)
	{
		_ASSERTE(mp_Grp1 && mp_Grp2);
		SafeRelease(mp_Grp1);
		SafeRelease(mp_Grp2);
		return nullptr;
	}

	// Параметры разбиения
	m_SplitType = aSplitType; // eSplitNone/eSplitHorz/eSplitVert
	mn_SplitPercent10 = std::max<UINT>(1, std::min<UINT>(anPercent10, 999)); // (0.1% - 99.9%)*10
	SetRectEmpty(&mrc_Splitter);
	SetRectEmpty(&mrc_DragSplitter);

	// Перенести в mp_Grp1 текущий VCon, mp_Grp2 - будет новый пустой (пока) панелью
	mp_Grp1->mp_Item = mp_Item;
	mp_Item->mp_Group = mp_Grp1;
	mp_Item = nullptr; // Отвязываемся от VCon, его обработкой теперь занимается mp_Grp1

	SetResizeFlags();

	return mp_Grp2;
}


CVirtualConsole* CVConGroup::CreateVCon(RConStartArgsEx& args, CVirtualConsole*& ppVConI, int index)
{
	_ASSERTE(ppVConI == nullptr);

	_ASSERTE(isMainThread()); // во избежание сбоев в индексах?

	if (args.pszSpecialCmd)
	{
		args.ProcessNewConArg();
	}

	if (args.ForceUserDialog == crb_On)
	{
		_ASSERTE(args.aRecreate!=cra_RecreateTab);
		args.aRecreate = cra_CreateTab;

		int nRc = gpConEmu->RecreateDlg(&args);
		if (nRc != IDC_START)
			return nullptr;

		// После диалога могли измениться параметры группы
	}

	void* pActiveGroupVConPtr = nullptr;
	CVConGroup* pGroup = nullptr;
	if (gp_VActive && args.eSplit)
	{
		CVConGuard VCon;
		if (((args.nSplitPane && GetVCon(gn_CreateGroupStartVConIdx + args.nSplitPane - 1, &VCon))
				|| (GetActiveVCon(&VCon) >= 0))
			&& VCon->mp_Group)
		{
			pGroup = ((CVConGroup*)VCon->mp_Group)->SplitVConGroup(args.eSplit, args.nSplitValue);
			if (pGroup)
			{
				pActiveGroupVConPtr = ((CVConGroup*)VCon->mp_Group)->mp_ActiveGroupVConPtr;
			}
		}

		_ASSERTE((pGroup!=nullptr) && "No active VCon?");
	}
	// Check
	if (!pGroup)
	{
		pGroup = CreateVConGroup();

		if (!pGroup)
			return nullptr;
	}

	#if defined(_DEBUG) && (SLEEP_BEFORE_VCON_CREATE > 0)
	Sleep(SLEEP_BEFORE_VCON_CREATE);
	#endif

	CVirtualConsole* pVCon = new CVirtualConsole(gpConEmu, index);
	ppVConI = pVCon;
	pGroup->mp_Item = pVCon;
	pGroup->mp_ActiveGroupVConPtr = pActiveGroupVConPtr ? pActiveGroupVConPtr : pVCon;
	pVCon->mp_Group = pGroup;

	//pVCon->Constructor(args);
	//if (!pVCon->mp_RCon->PreCreate(args))

	if (!pVCon->Constructor(&args))
	{
		ppVConI = nullptr;
		bool bWasValid = isValid(pVCon);

		if (!bWasValid)
		{
			CVConGroup* pClosedGroup = ((CVConGroup*)pVCon->mp_Group);
			pClosedGroup->RemoveGroup();
		}

		CVConGroup::OnVConClosed(pVCon);

		if (!bWasValid)
		{
			// If the VCon was not valid before OnVConClosed, it will not be released
			// But we need to release it here to avoid mem leaks
			// _ASSERTE(pVCon->RefCount() == 1); -- that may not be true. It can be temporarily locked in other threads (from isVisible calls for ex).
			pVCon->Release();
		}
		else
		{
			#ifdef _DEBUG
				//-- must be already released in CVConGroup::OnVConClosed!
				#ifndef _WIN64
				_ASSERTE((DWORD_PTR)pVCon->mp_RCon==0xFEEEFEEE);
				#else
				_ASSERTE((DWORD_PTR)pVCon->mp_RCon==0xFEEEFEEEFEEEFEEELL);
				#endif
				//pVCon->Release();
			#endif
		}
		return nullptr;
	}

	setActiveVConAndFlags(gp_VActive);

	pGroup->GetRootGroup()->InvalidateAll();

	return pVCon;
}


CVConGroup::CVConGroup(CVConGroup *apParent)
{
	mb_Released = false;
	mp_Item = nullptr/*apVCon*/;     // консоль, к которой привязан этот "Pane"
	//apVCon->mp_Group = this;
	m_SplitType = RConStartArgsEx::eSplitNone;
	mn_SplitPercent10 = 500; // Default - пополам
	mrc_Full = MakeRect(0,0);
	mrc_Splitter = MakeRect(0,0);
	mp_Grp1 = mp_Grp2 = nullptr; // Ссылки на "дочерние" панели
	mp_Parent = apParent; // Ссылка на "родительскую" панель
	mb_ResizeFlag = false;
	ZeroStruct(mrc_DragSplitter);
	mp_ActiveGroupVConPtr = nullptr;
	mb_GroupInputFlag = apParent ? apParent->mb_GroupInputFlag : false;
	mb_PaneMaximized = apParent ? apParent->mb_PaneMaximized : false;


	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	bool bAdded = false;
	for (size_t i = 0; i < countof(gp_VGroups); i++)
	{
		if (gp_VGroups[i] == nullptr)
		{
			gp_VGroups[i] = this;
			bAdded = true;
			break;
		}
	}
	_ASSERTE(bAdded && "gp_VGroups overflow");

	lockGroups.Unlock();
}

void CVConGroup::FinalRelease()
{
	MCHKHEAP;
	_ASSERTE(isMainThread());
	CVConGroup* pGroup = (CVConGroup*)this;
	delete pGroup;
	MCHKHEAP;
};

void CVConGroup::RemoveGroup()
{
	if (InterlockedExchange(&mb_Released, TRUE))
		return;

	_ASSERTE(isMainThread()); // во избежание сбоев в индексах?

	// Не должно быть дочерних панелей
	_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);


	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	if (mp_Parent)
	{
		// Если есть родитель - то нужно перекинуть "AnotherPane" в родителя,
		// чтобы не было разрывов в дереве панелей.
		CVConGroup* p = GetAnotherGroup();
		if (!p)
		{
			_ASSERTE(p); //-V571
		}
		else
		{
			p->MoveToParent(mp_Parent);
			p->Release();
		}
	}

	bool bRemoved = false;
	for (size_t i = 0; i < countof(gp_VGroups); i++)
	{
		if (gp_VGroups[i] == this)
		{
			gp_VGroups[i] = nullptr;
			_ASSERTE(!bRemoved);
			bRemoved = true;
		}
	}
	_ASSERTE(bRemoved && "Was not pushed in gp_VGroups?");

	lockGroups.Unlock();
}

CVConGroup::~CVConGroup()
{
	_ASSERTE(isMainThread()); // во избежание сбоев в индексах?

	if (mp_GroupSplitDragging == this)
	{
		StopSplitDragging();
		mp_GroupSplitDragging = nullptr;
	}

	if (!mb_Released)
		RemoveGroup();
}

void CVConGroup::OnVConDestroyed(CVirtualConsole* apVCon)
{
	if (apVCon && apVCon->mp_Group)
	{
		CVConGroup* p = (CVConGroup*)apVCon->mp_Group;
		apVCon->mp_Group = nullptr;
		p->Release();
	}
}

CVConGroup* CVConGroup::GetRootGroup() const
{
	if (!this)
	{
		_ASSERTE(FALSE);
		return nullptr;
	}

	auto p = this;
	while (p->mp_Parent)
	{
		p = p->mp_Parent;
	}

	_ASSERTE(p && (p->mp_Parent == nullptr));
	return const_cast<CVConGroup*>(p);
}

CVConGroup* CVConGroup::GetRootOfVCon(CVirtualConsole* apVCon)
{
	if (!apVCon)
	{
		_ASSERTE(apVCon != nullptr);
		return nullptr;
	}

	CVConGuard VCon(apVCon);

	if (!apVCon || !apVCon->mp_Group)
	{
		_ASSERTE(apVCon && apVCon->mp_Group);
		return nullptr;
	}

	CVConGroup* p = ((CVConGroup*)apVCon->mp_Group)->GetRootGroup();
	return p;
}

CVConGroup* CVConGroup::GetAnotherGroup() const
{
	if (!this)
	{
		_ASSERTE(FALSE);
		return nullptr;
	}

	if (!mp_Parent)
	{
		return nullptr;
	}

	auto p = (mp_Parent->mp_Grp1 == this) ? mp_Parent->mp_Grp2 : mp_Parent->mp_Grp1;
	_ASSERTE(p && p != this && p->mp_Parent == mp_Parent);
	return const_cast<CVConGroup*>(p);
}

void CVConGroup::SetResizeFlags()
{
	AssertThis();

	CVConGroup* p = GetRootGroup();
	if (p)
	{
		p->mb_ResizeFlag = true;
	}
}

void CVConGroup::MoveToParent(CVConGroup* apParent)
{
	// По идее пока только так.
	_ASSERTE(apParent && apParent == mp_Parent);

	// Не должно быть И VCon и разбиения на группы
	_ASSERTE((mp_Item!=nullptr) != (mp_Grp1!=nullptr || mp_Grp2!=nullptr));

	apParent->SetResizeFlags();

	apParent->mp_Item = mp_Item;
	apParent->m_SplitType = m_SplitType; // eSplitNone/eSplitHorz/eSplitVert
	apParent->mn_SplitPercent10 = mn_SplitPercent10; // (0.1% - 99.9%)*10
	apParent->mrc_Full = mrc_Full;
	apParent->mrc_Splitter = mrc_Splitter;

	// Ссылки на "дочерние" панели
	apParent->mp_Grp1 = mp_Grp1;
	apParent->mp_Grp2 = mp_Grp2;
	// Ссылки на "родительскую" панель
	if (mp_Grp1)
	{
		mp_Grp1->mp_Parent = apParent;
		mp_Grp1 = nullptr;
	}
	if (mp_Grp2)
	{
		mp_Grp2->mp_Parent = apParent;
		mp_Grp2 = nullptr;
	}

	// VCon
	if (mp_Item)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);
		mp_Item->mp_Group = apParent;
		mp_Item = nullptr; // Отвязываемся от VCon, его обработкой теперь занимается mp_Grp1
	}

	mp_Parent = nullptr;
}

void CVConGroup::GetAllTextSize(SIZE& sz, SIZE& splits, const bool abMinimal /*= false*/)
{
	sz.cx = MIN_CON_WIDTH;
	sz.cy = MIN_CON_HEIGHT;

	AssertThis();

	_ASSERTE((m_SplitType==RConStartArgsEx::eSplitNone) == (mp_Grp1==nullptr && mp_Grp2==nullptr && mp_Item!=nullptr));
	if (m_SplitType==RConStartArgsEx::eSplitNone)
	{
		const CVConGuard VCon(mp_Item);
		if (!abMinimal && mp_Item && mp_Item->RCon())
		{
			sz.cx = mp_Item->RCon()->TextWidth();
			sz.cy = mp_Item->RCon()->TextHeight();
		}
		else
		{
			_ASSERTE(mp_Item!=nullptr);
			sz.cx = MIN_CON_WIDTH;
			sz.cy = MIN_CON_HEIGHT;
		}
	}
	else
	{
		CVConGuard VCon1(mp_Grp1 ? mp_Grp1->mp_Item : nullptr);
		CVConGuard VCon2(mp_Grp2 ? mp_Grp2->mp_Item : nullptr);
		SIZE sz1 = {MIN_CON_WIDTH,MIN_CON_HEIGHT}, sz2 = {MIN_CON_WIDTH,MIN_CON_HEIGHT};

		if (mp_Grp1)
		{
			mp_Grp1->GetAllTextSize(sz1, splits, abMinimal);
		}
		else
		{
			_ASSERTE(mp_Grp1!=nullptr);
		}

		sz = sz1;

		if (mp_Grp2 /*&& (m_SplitType == RConStartArgsEx::eSplitHorz)*/)
		{
			mp_Grp2->GetAllTextSize(sz2, splits, abMinimal);
		}
		else
		{
			_ASSERTE(mp_Grp2!=nullptr);
		}

		// Add second pane
		if (m_SplitType == RConStartArgsEx::eSplitHorz)
		{
			sz.cx += sz2.cx;
			splits.cx++;
		}
		else if (m_SplitType == RConStartArgsEx::eSplitVert)
		{
			sz.cy += sz2.cy;
			splits.cy++;
		}
		else
		{
			_ASSERTE((m_SplitType == RConStartArgsEx::eSplitHorz) || (m_SplitType == RConStartArgsEx::eSplitVert));
		}
	}

	return;
}

void CVConGroup::LogString(LPCSTR asText)
{
	if (gpSet->isLogging() && gp_VActive)
		gp_VActive->RCon()->LogString(asText);
}

void CVConGroup::LogString(LPCWSTR asText)
{
	if (gpSet->isLogging() && gp_VActive)
		gp_VActive->RCon()->LogString(asText);
}

void CVConGroup::LogInput(UINT uMsg, WPARAM wParam, LPARAM lParam, LPCWSTR pszTranslatedChars /*= nullptr*/)
{
	if (gpSet->isLogging() && gp_VActive)
		gp_VActive->RCon()->LogInput(uMsg, wParam, lParam, pszTranslatedChars);
}

void CVConGroup::StopSignalAll()
{
	MCHKHEAP;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i])
		{
			gp_VCon[i]->RCon()->StopSignal();
		}
	}
}

void CVConGroup::DestroyAllVCon()
{
	MCHKHEAP;
	CVConGuard VCon;
	for (size_t i = countof(gp_VCon); i--;)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			CVirtualConsole* p = VCon.VCon();
			gp_VCon[i] = nullptr;
			VCon.Release();
			p->Release();
		}
	}
	MCHKHEAP;
}

void CVConGroup::OnDestroyConEmu()
{
	// Нужно проверить, вдруг окно закрылось без нашего ведома (InsideIntegration)
	CVConGuard VCon;
	for (size_t i = countof(gp_VCon); i--;)
	{
		if (VCon.Attach(gp_VCon[i]) && VCon->RCon())
		{
			if (!VCon->RCon()->isDetached()
				&& VCon->RCon()->ConWnd())
			{
				VCon->RCon()->DetachRCon(true, true);
			}
		}
	}
}

void CVConGroup::OnAlwaysShowScrollbar(bool abSync /*= true*/)
{
	struct impl
	{
		static bool ShowScroll(CVirtualConsole* pVCon, LPARAM lParam)
		{
			pVCon->OnAlwaysShowScrollbar(lParam!=0);
			return true;
		};
	};
	EnumVCon(evf_All, impl::ShowScroll, static_cast<LPARAM>(abSync));
}

void CVConGroup::RepositionVCon(RECT rcNewCon, bool bVisible)
{
	AssertThis();

	bool lbPosChanged = false;
	RECT rcCurCon = {}, rcCurBack = {};

	CVConGuard VConI(mp_Item);
	if (m_SplitType == RConStartArgsEx::eSplitNone)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);

		//RECT rcScroll = gpConEmu->CalcMargins(CEM_SCROLL);
		//gpConEmu->AddMargins(rcNewCon, rcScroll);

		HWND hWndDC = mp_Item ? VConI->GetView() : nullptr;
		HWND hBack = mp_Item ? VConI->GetBack() : nullptr;
		if (hWndDC)
		{
			GetWindowRect(hWndDC, &rcCurCon);
			GetWindowRect(hBack, &rcCurBack);

			_ASSERTE(GetParent(hWndDC) == ghWnd && ghWnd);
			MapWindowPoints(nullptr, ghWnd, (LPPOINT)&rcCurCon, 2);
			MapWindowPoints(nullptr, ghWnd, (LPPOINT)&rcCurBack, 2);

			RECT rcDC = gpConEmu->CalcRect(CER_DC, rcNewCon, CER_BACK, VConI.VCon());

			if (bVisible)
			{
				lbPosChanged = memcmp(&rcCurCon, &rcDC, sizeof(RECT))!=0
					|| memcmp(&rcCurBack, &rcNewCon, sizeof(RECT))!=0;
			}
			else
			{
				// Here we take care only about upper-left corner (X/Y)
				lbPosChanged = memcmp(&rcCurCon, &rcDC, sizeof(POINT))!=0 //-V512
					|| memcmp(&rcCurBack, &rcNewCon, sizeof(RECT))!=0;
			}

			// Двигаем/ресайзим
			if (lbPosChanged)
			{
				VConI->SetVConSizePos(rcNewCon, bVisible);
				//if (bVisible)
				//{
				//	// Двигаем/ресайзим окошко DC
				//	MoveWindow(hWndDC, rcNewCon.left, rcNewCon.top, rcNewCon.right - rcNewCon.left, rcNewCon.bottom - rcNewCon.top, 1);
				//	VConI->Invalidate();
				//}
				//else
				//{
				//	// Двигаем окошко DC
				//	SetWindowPos(hWndDC, nullptr, rcNewCon.left, rcNewCon.top, 0,0, SWP_NOSIZE|SWP_NOZORDER);
				//}
			}
		}

		mrc_Full = rcNewCon;

		if (VConI->RCon()->GuiWnd())
		{
			VConI->RCon()->SyncGui2Window(rcNewCon);
		}
	}
	else if (mp_Grp1 && mp_Grp2)
	{
		RECT rcCon1, rcCon2, rcSplitter = {};
		if (mb_PaneMaximized)
		{
			rcCon1 = rcCon2 = rcNewCon;
		}
		else
		{
			CalcSplitRect(mn_SplitPercent10, rcNewCon, rcCon1, rcCon2, rcSplitter);
		}

		mrc_Full = rcNewCon;
		mrc_Splitter = rcSplitter;

		mp_Grp1->RepositionVCon(rcCon1, bVisible);
		mp_Grp2->RepositionVCon(rcCon2, bVisible);
	}
	else
	{
		_ASSERTE(mp_Grp1 && mp_Grp2);
	}
}

bool CVConGroup::ReSizeSplitter(int iCells)
{
	bool bChanged = false;

	int nSaveSplitPercent10 = mn_SplitPercent10;

	int nCellSize = (m_SplitType == RConStartArgsEx::eSplitVert) ? gpFontMgr->FontHeight() : gpFontMgr->FontWidth();
	int nMinCells = (m_SplitType == RConStartArgsEx::eSplitVert) ? MIN_CON_HEIGHT : MIN_CON_WIDTH;

	// Нашли? Корректируем mn_SplitPercent10
	// mp_Item может быть nullptr если этот mp_Grp в свою очередь разбит
	if (mp_Grp1 && mp_Grp2 && (nCellSize > 0))
	{
		RECT rcCon1 = {0}, rcCon2 = {0}, rcSplitter = {0};
		CalcSplitRect(mn_SplitPercent10, mrc_Full, rcCon1, rcCon2, rcSplitter);

		RECT rcScroll = gpConEmu->CalcMargins(CEM_SCROLL|CEM_PAD);

		int iPadSize = (m_SplitType == RConStartArgsEx::eSplitVert) ? (rcScroll.top + rcScroll.bottom) : (rcScroll.left + rcScroll.right);
		int iSize1 = ((m_SplitType == RConStartArgsEx::eSplitVert) ? mrc_Splitter.top-mrc_Full.top : mrc_Splitter.left-mrc_Full.left) - iPadSize;
		int iSize2 = ((m_SplitType == RConStartArgsEx::eSplitVert) ? mrc_Full.bottom-mrc_Splitter.bottom : mrc_Full.right-mrc_Splitter.right) - iPadSize;
		int iCellHalf = std::max(1,(nCellSize/2)) * (iCells > 0 ? 1 : -1);
		int iDiff = iCells*nCellSize - iCellHalf;

		bool bCanResize = (iCells < 0)
			? (iSize1 >= (nCellSize*(nMinCells+1)))
			: (iSize2 >= (nCellSize*(nMinCells+1)));
		if (bCanResize)
		{
			RECT rcNewCon1 = {0}, rcNewCon2 = {0}, rcNewSplitter = {0};
			int iNewSize1 = iSize1 + iDiff, iNewSize2 = iSize2 - iDiff;
			for (int nTries = 20; nTries > 0; nTries--)
			{
				// Считаем новый процент
				int nNewSplitPercent10 = (iNewSize1 * 1000 / (iNewSize1 + iNewSize2));

				// Do not try to calc if nNewSplitPercent10 was not changed into desired direction
				if ((nNewSplitPercent10 > 0) && (nNewSplitPercent10 <= 999)
					&& ((iCells < 0) == ((UINT)nNewSplitPercent10 < mn_SplitPercent10)))
				{
					int nOldPercent = mn_SplitPercent10;
					mn_SplitPercent10 = std::max(1,std::min(nNewSplitPercent10,999)); // (0.1% - 99.9%)*10

					CalcSplitRect(mn_SplitPercent10, mrc_Full, rcNewCon1, rcNewCon2, rcNewSplitter);

					int iChanged = ((m_SplitType == RConStartArgsEx::eSplitVert) ? rcNewSplitter.top : rcNewSplitter.left) - ((m_SplitType == RConStartArgsEx::eSplitVert) ? rcSplitter.top : rcSplitter.left);
					int iCellsChanged = iChanged / nCellSize;

					bChanged = (m_SplitType == RConStartArgsEx::eSplitVert) ? (rcNewSplitter.top != rcSplitter.top) : (rcNewSplitter.left != rcSplitter.left);
					if (bChanged)
						break;
				}

				// Из-за округлений, отступов и просветов на краях - могли не попасть в кратность ячейкам
				iNewSize1 += iCellHalf; iNewSize2 -= iCellHalf;
			}

			// Если размер консоли окажется менее минимального - откатим
			if (!bChanged
				|| ((m_SplitType == RConStartArgsEx::eSplitVert)
					&& (((rcNewCon1.bottom-rcNewCon1.top) < (iPadSize+nCellSize*nMinCells))
						|| ((rcNewCon2.bottom-rcNewCon2.top) < (iPadSize+nCellSize*nMinCells))))
				|| ((m_SplitType == RConStartArgsEx::eSplitHorz)
					&& (((rcNewCon1.right-rcNewCon1.left) < (iPadSize+nCellSize*nMinCells))
						|| ((rcNewCon2.right-rcNewCon2.left) < (iPadSize+nCellSize*nMinCells))))
				)
			{
				mn_SplitPercent10 = nSaveSplitPercent10;
				bChanged = false;
			}
			else
			{
				bChanged = (nSaveSplitPercent10 != mn_SplitPercent10);
			}
		}
	}

	return bChanged;
}

struct ReSizeSplitterHelperArg
{
	CVirtualConsole* pVCon;
	int iHorz /*= 0*/;
	int iVert /*= 0*/;

	ReSizeSplitterHelperArg(CVirtualConsole* apVCon, int aiHorz, int aiVert)
	{
		pVCon = apVCon; iHorz = aiHorz; iVert = aiVert;
	};
};

LPARAM CVConGroup::ReSizeSplitterHelper(LPARAM lParam)
{
	ReSizeSplitterHelperArg* p = (ReSizeSplitterHelperArg*)lParam;
	ReSizeSplitter(p->pVCon, p->iHorz, p->iVert);
	delete p;
	return 0;
}

bool CVConGroup::ReSizeSplitter(CVirtualConsole* apVCon, int iHorz /*= 0*/, int iVert /*= 0*/)
{
	if (!apVCon || (!iHorz && !iVert))
	{
		_ASSERTE(apVCon && (iHorz || iVert));
		return false;
	}

	if (!isMainThread())
	{
		ReSizeSplitterHelperArg* p = new ReSizeSplitterHelperArg(apVCon, iHorz, iVert);
		apVCon->mp_ConEmu->CallMainThread(false, ReSizeSplitterHelper, reinterpret_cast<LPARAM>(p));
		return false;
	}

	// Валидна, или успела закрыться?
	if (!isValid(apVCon))
	{
		return false;
	}

	CVConGuard VCon(apVCon);
	if (!apVCon->mp_Group)
	{
		_ASSERTE(apVCon->mp_Group); //-V571
		return false;
	}

	bool bChanged = false;

	CVConGroup* p = (CVConGroup*)apVCon->mp_Group;

	if (p->mb_PaneMaximized)
	{
		gpConEmu->LogString(L"ReSizeSplitter skipped because of mb_PaneMaximized");
		return false;
	}

	if (iHorz != 0)
	{
		// Нужно найти ближайшую группу, разделенную слева-направо
		CVConGroup* ps = p;
		while (ps && (ps->m_SplitType != RConStartArgsEx::eSplitHorz))
		{
			ps = ps->mp_Parent;
		}
		// Нашли? Корректируем mn_SplitPercent10
		if (ps && ps->ReSizeSplitter(iHorz))
		{
			bChanged = true;
		}
	}

	if (iVert != 0)
	{
		// Нужно найти ближайшую группу, разделенную сверху-вниз
		CVConGroup* ps = p;
		while (ps && (ps->m_SplitType != RConStartArgsEx::eSplitVert))
		{
			ps = ps->mp_Parent;
		}
		// Нашли? Корректируем mn_SplitPercent10
		if (ps && ps->ReSizeSplitter(iVert))
		{
			bChanged = true;
		}
	}

	if (bChanged)
	{
		RECT workspace = apVCon->mp_ConEmu->WorkspaceRect();
		CVConGroup::SyncAllConsoles2Window(workspace);
		CVConGroup::ReSizePanes(workspace);
	}

	return bChanged;
}

// Разбиение в координатах DC (pixels)
void CVConGroup::CalcSplitRect(UINT nSplitPercent10, RECT rcNewCon, RECT& rcCon1, RECT& rcCon2, RECT& rcSplitter) const
{
	rcCon1 = rcNewCon;
	rcCon2 = rcNewCon;
	rcSplitter = MakeRect(0,0);

	AssertThis();

	// Split is Maximized?
	if (mb_PaneMaximized)
	{
		return;
	}

	// Заблокируем заранее
	CVConGuard VCon1(mp_Grp1 ? mp_Grp1->mp_Item : nullptr);
	CVConGuard VCon2(mp_Grp2 ? mp_Grp2->mp_Item : nullptr);

	if ((m_SplitType == RConStartArgsEx::eSplitNone) || !mp_Grp1 || !mp_Grp2)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);
		_ASSERTE((m_SplitType != RConStartArgsEx::eSplitNone) && "Need no split");
		return;
	}

	WARNING("Не учитывается gpSet->nCenterConsolePad?");

	UINT nSplit = std::max<UINT>(1, std::min<UINT>(nSplitPercent10, 999));

	if (m_SplitType == RConStartArgsEx::eSplitHorz)
	{
		UINT nWidth = rcNewCon.right - rcNewCon.left;

		UINT nPadX = gpSetCls->EvalSize(gpSet->nSplitWidth, esf_Horizontal|esf_CanUseDpi);
		if (nWidth >= nPadX)
			nWidth -= nPadX;
		else
			nPadX = 0;

		RECT rcScroll = gpConEmu->CalcMargins(CEM_SCROLL|CEM_PAD);
		if (rcScroll.right || rcScroll.left)
		{
			if (nWidth > (UINT)((rcScroll.right + rcScroll.left) * 2))
				nWidth -= (rcScroll.right + rcScroll.left) * 2;
			else
				rcScroll.right = 0;
		}

		UINT nScreenWidth = (nWidth * nSplit / 1000);
		LONG nCellWidth = gpFontMgr->FontWidth();
		LONG nCon2Width;
		if (nCellWidth > 0)
		{
			UINT nTotalCellCountX = nWidth / nCellWidth;
			UINT nCellCountX = (nScreenWidth + (nCellWidth/2)) / nCellWidth;
			if ((nTotalCellCountX >= 2) && (nCellCountX >= nTotalCellCountX))
			{
				_ASSERTE(FALSE && "Too small rect?");
				nCellCountX = nTotalCellCountX - 1;
			}
			nScreenWidth = nCellCountX * nCellWidth + (rcScroll.right + rcScroll.left);
			nCon2Width = (nTotalCellCountX - nCellCountX) * nCellWidth + (rcScroll.right + rcScroll.left);
			_ASSERTE(nCon2Width > 0);
		}
		else
		{
			_ASSERTE(nCellWidth > 0);
			nCon2Width = rcNewCon.right - (rcCon1.right+nPadX+rcScroll.right);
			_ASSERTE(nCon2Width > 0);
		}

		rcCon1 = MakeRect(rcNewCon.left, rcNewCon.top,
			std::max(rcNewCon.left + nScreenWidth,rcNewCon.right - nCon2Width - nPadX), rcNewCon.bottom);
		rcCon2 = MakeRect(rcNewCon.right - nCon2Width, rcNewCon.top, rcNewCon.right, rcNewCon.bottom);
		rcSplitter = MakeRect(rcCon1.right, rcCon1.top, rcCon2.left, rcCon2.bottom);
	}
	else // RConStartArgsEx::eSplitVert
	{
		UINT nHeight = rcNewCon.bottom - rcNewCon.top;

		UINT nPadY = gpSetCls->EvalSize(gpSet->nSplitHeight, esf_Vertical|esf_CanUseDpi);
		if (nHeight >= nPadY)
			nHeight -= nPadY;
		else
			nPadY = 0;

		RECT rcScroll = gpConEmu->CalcMargins(CEM_SCROLL|CEM_PAD);
		if (rcScroll.bottom || rcScroll.top)
		{
			if (nHeight > (UINT)((rcScroll.bottom + rcScroll.top) * 2))
				nHeight -= (rcScroll.bottom + rcScroll.top) * 2;
			else
				rcScroll.bottom = 0;
		}

		UINT nScreenHeight = (nHeight * nSplit / 1000);
		LONG nCellHeight = gpFontMgr->FontHeight();
		LONG nCon2Height;
		if (nCellHeight > 0)
		{
			UINT nTotalCellCountY = nHeight / nCellHeight;
			UINT nCellCountY = (nScreenHeight + (nCellHeight/2)) / nCellHeight;
			if ((nTotalCellCountY >= 2) && (nCellCountY >= nTotalCellCountY))
			{
				_ASSERTE(FALSE && "Too small rect?");
				nCellCountY = nTotalCellCountY - 1;
			}
			nScreenHeight = nCellCountY * nCellHeight + (rcScroll.bottom + rcScroll.top);
			nCon2Height = (nTotalCellCountY - nCellCountY) * nCellHeight + (rcScroll.bottom + rcScroll.top);
			_ASSERTE(nCon2Height > 0);
		}
		else
		{
			_ASSERTE(nCellHeight > 0);
			nCon2Height = rcNewCon.bottom - (rcCon1.bottom+nPadY+rcScroll.bottom);
		}

		rcCon1 = MakeRect(rcNewCon.left, rcNewCon.top,
			rcNewCon.right, std::max(rcNewCon.top + nScreenHeight,rcNewCon.bottom - nCon2Height - nPadY));
		rcCon2 = MakeRect(rcCon1.left, rcNewCon.bottom - nCon2Height, rcNewCon.right, rcNewCon.bottom);
		rcSplitter = MakeRect(rcCon1.left, rcCon1.bottom, rcCon2.right, rcCon2.top);
	}
}

// Evaluate rect of exact group (pTarget) from root rectange (CER_WORKSPACE)
void CVConGroup::CalcSplitRootRect(const RECT rcAll, RECT& rcCon, const CVConGroup* pTarget /*= nullptr*/) const
{
	rcCon = rcAll;
	AssertThis();

	if (!mp_Parent && !pTarget || mb_PaneMaximized)
	{
		return;
	}

	RECT rc = rcAll;
	if (mp_Parent)
	{
		_ASSERTE(pTarget != this);
		mp_Parent->CalcSplitRootRect(rcAll, rc, this);
	}

	if (m_SplitType == RConStartArgsEx::eSplitNone)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);
		rcCon = rc;
	}
	else
	{
		RECT rc1, rc2, rcSplitter;
		CalcSplitRect(mn_SplitPercent10, rc, rc1, rc2, rcSplitter);

		_ASSERTE(pTarget == mp_Grp1 || pTarget == mp_Grp2);
		rcCon = (pTarget == mp_Grp2) ? rc2 : rc1;
	}
}

RConStartArgsEx::SplitType CVConGroup::isSplitterDragging()
{
	if (mp_GroupSplitDragging && !isPressed(VK_LBUTTON))
	{
		StopSplitDragging();
	}

	return (mp_GroupSplitDragging != nullptr) ? mp_GroupSplitDragging->m_SplitType : RConStartArgsEx::eSplitNone;
}

LRESULT CVConGroup::OnMouseEvent(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	_ASSERTE(isMainThread());

	LRESULT lRc = 0;
	CVConGroup* pGrp = nullptr;
	POINT pt;
	HCURSOR hCur = nullptr;
	UINT nPrevSplit;
	UINT nNewSplit;
	RECT rcNewSplit;

	GetCursorPos(&pt);
	MapWindowPoints(nullptr, ghWnd, &pt, 1);

	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	switch (uMsg)
	{
	case WM_SETCURSOR:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		pGrp = mp_GroupSplitDragging ? mp_GroupSplitDragging : FindSplitGroup(pt, nullptr);
		break;

	case WM_LBUTTONUP:
	case WM_MOUSEMOVE:
		if (isGroupVisible(mp_GroupSplitDragging))
			pGrp = mp_GroupSplitDragging;
		else
			StopSplitDragging();
		break;
	}

	if (!pGrp)
		StopSplitDragging();

	if (uMsg == WM_SETCURSOR)
	{
		if (!pGrp || pGrp->m_SplitType == RConStartArgsEx::eSplitNone)
			hCur = gpConEmu->mh_CursorArrow;
		else if (pGrp->m_SplitType == RConStartArgsEx::eSplitHorz)
			hCur = gpConEmu->mh_SplitH;
		else if (pGrp->m_SplitType == RConStartArgsEx::eSplitVert)
			hCur = gpConEmu->mh_SplitV;

		if (hCur) // Must not be nullptr, otherwise cursor will be hidden
			SetCursor(hCur);

		lRc = (hCur != gpConEmu->mh_CursorArrow);
		goto wrap;
	}

	// No splitter - no actions
	if (!pGrp || (pGrp->m_SplitType == RConStartArgsEx::eSplitNone))
		goto wrap;

	nNewSplit = nPrevSplit = pGrp->mn_SplitPercent10;
	rcNewSplit = pGrp->mrc_DragSplitter;

	switch (uMsg)
	{
	case WM_LBUTTONDBLCLK:
		nNewSplit = 500; // Reset at center
		break;

	case WM_LBUTTONDOWN:
		mp_GroupSplitDragging = pGrp;
		SetCapture(hWnd);
		break;

	case WM_LBUTTONUP:
	case WM_MOUSEMOVE:
		if (mp_GroupSplitDragging)
		{
			int nAllSize = (pGrp->m_SplitType == RConStartArgsEx::eSplitVert)
				? (pGrp->mrc_Full.bottom - pGrp->mrc_Full.top - (pGrp->mrc_Splitter.bottom - pGrp->mrc_Splitter.top))
				: (pGrp->mrc_Full.right - pGrp->mrc_Full.left - (pGrp->mrc_Splitter.right - pGrp->mrc_Splitter.left));
			int nNewPos = (pGrp->m_SplitType == RConStartArgsEx::eSplitVert)
				? (pt.y - pGrp->mrc_Full.top)
				: (pt.x - pGrp->mrc_Full.left);
			// Need to consider scrollbars and pads?
			if ((nAllSize > 0) && (nNewPos > 0) && (nNewPos < nAllSize))
			{
				nNewSplit = (nNewPos * 1000) / nAllSize;
			}
		}
		break;
	}

	// New desired splitter position (for PatBlt)
	if ((nNewSplit >= 1) && (nNewSplit <= 999) && (nNewSplit != nPrevSplit))
	{
		#if 0
		if (pGrp->m_SplitType == RConStartArgsEx::eSplitVert)
		{
			int nNewPos = pt.y - pGrp->mrc_Full.top;
			int nHeight = pGrp->mrc_Splitter.bottom - pGrp->mrc_Splitter.top;
			rcNewSplit = MakeRect(pGrp->mrc_Splitter.left, nNewPos, pGrp->mrc_Splitter.right, nNewPos + nHeight);
		}
		else
		{
			int nNewPos = pt.x - pGrp->mrc_Full.left;
			int nWidth = pGrp->mrc_Splitter.right - pGrp->mrc_Splitter.left;
			rcNewSplit = MakeRect(nNewPos, pGrp->mrc_Splitter.top, nNewPos+nWidth, pGrp->mrc_Splitter.bottom);
		}
		#endif
		pGrp->mn_SplitPercent10 = nNewSplit;
		gpConEmu->OnSize(false);
	}


	if ((uMsg == WM_LBUTTONUP) || !isPressed(VK_LBUTTON))
	{
		StopSplitDragging();
	}
wrap:
	return lRc;
}

void CVConGroup::StopSplitDragging()
{
	if (mp_GroupSplitDragging)
	{
		bool bChanged = false;
		CGroupGuard Grp(mp_GroupSplitDragging);
		if (!IsRectEmpty(&mp_GroupSplitDragging->mrc_DragSplitter))
		{
			RECT rcFull = mp_GroupSplitDragging->mrc_Full;
			RECT rcSplit = mp_GroupSplitDragging->mrc_DragSplitter;
			int nAllSize = (mp_GroupSplitDragging->m_SplitType == RConStartArgsEx::eSplitVert)
				? (rcFull.bottom - rcFull.top - (rcSplit.bottom - rcSplit.top))
				: (rcFull.right - rcFull.left - (rcSplit.right - rcSplit.left));
			int nNewPos = (mp_GroupSplitDragging->m_SplitType == RConStartArgsEx::eSplitVert)
				? (rcSplit.top - rcFull.top)
				: (rcSplit.left - rcFull.left);
			// Need to consider scrollbars and pads?
			if ((nAllSize > 0) && (nNewPos > 0) && (nNewPos < nAllSize))
			{
				UINT nNewSplit = (nNewPos * 1000) / nAllSize;
				if ((nNewSplit >= 1) && (nNewSplit <= 999) && (nNewSplit != mp_GroupSplitDragging->mn_SplitPercent10))
				{
					mp_GroupSplitDragging->mn_SplitPercent10 = nNewSplit;
					bChanged = true;
				}
			}

			SetRectEmpty(&mp_GroupSplitDragging->mrc_DragSplitter);
		}
		mp_GroupSplitDragging = nullptr;
		ReleaseCapture();
		gpConEmu->OnSize();
	}
}

bool CVConGroup::isGroupVisible(CVConGroup* pGrp)
{
	if (!pGrp)
		return false;
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	CVConGroup* p = GetRootOfVCon(gp_VActive);
	return (p == pGrp->GetRootGroup());
}

CVConGroup* CVConGroup::FindSplitGroup(POINT ptWork, CVConGroup* pFrom)
{
	// gpcs_VGroups must be locked by caller!
	CVConGroup* pGrp = pFrom ? pFrom : gp_VActive ? GetRootOfVCon(gp_VActive) : nullptr;

	if (!pGrp || (pGrp->m_SplitType == RConStartArgsEx::eSplitNone))
		return nullptr;

	if (PtInRect(&pGrp->mrc_Splitter, ptWork))
		return pGrp;

	if (!PtInRect(&pGrp->mrc_Full, ptWork))
		return nullptr;

	CVConGroup* pFind;
	if (pGrp->mp_Grp1 && ((pFind = FindSplitGroup(ptWork, pGrp->mp_Grp1)) != nullptr))
		return pFind;
	if (pGrp->mp_Grp2 && ((pFind = FindSplitGroup(ptWork, pGrp->mp_Grp2)) != nullptr))
		return pFind;

	return nullptr;
}

void CVConGroup::ShowAllVCon(int nShowCmd)
{
	AssertThis();

	CVConGuard VConI(mp_Item);

	if (m_SplitType == RConStartArgsEx::eSplitNone)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);
		if (VConI.VCon())
		{
			if (nShowCmd && mb_PaneMaximized && !VConI->isVisible())
				nShowCmd = SW_HIDE;
			VConI->ShowView(nShowCmd);
		}
	}
	else if (mp_Grp1 && mp_Grp2)
	{
		mp_Grp1->ShowAllVCon(nShowCmd);
		mp_Grp2->ShowAllVCon(nShowCmd);
	}
	else
	{
		_ASSERTE(mp_Grp1 && mp_Grp2);
	}
}

void CVConGroup::MoveAllVCon(CVirtualConsole* pVConCurrent, RECT rcNewCon)
{
	CVConGroup* pGroup = GetRootOfVCon(pVConCurrent);
	if (!pGroup)
	{
		_ASSERTE(pGroup); //-V571
		return;
	}

	//bool lbPosChanged = false;
	//RECT rcCurCon = {};
	CVConGroup* pRoots[MAX_CONSOLE_COUNT+1] = {pGroup};
	CVConGuard VCon(pVConCurrent);

	WARNING("DoubleView и не только: Переделать. Ресайз должен жить в ConEmuChild/VConGroup!");

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VConI(gp_VCon[i]);
		if (!VConI.VCon())
			break;
		if (VConI.VCon() == pVConCurrent)
			continue;
		// Обрабатывать "по группам", а не "по консолям"
		bool bProcessed = false;
		CVConGroup* pCurGroup = GetRootOfVCon(VConI.VCon());
		size_t j = 0;
		while (pRoots[j])
		{
			if (pRoots[j] == pCurGroup)
			{
				bProcessed = true; break;
			}
			j++;
		}
		if (bProcessed)
			continue; // уже
		pRoots[j] = pCurGroup; // Запомним, однократно

		// Двигаем окошко DC
		pCurGroup->RepositionVCon(rcNewCon, false);

		//// Двигаем
		//HWND hWndDC = VConI->GetView();
		//if (hWndDC)
		//{
		//	WARNING("DoubleView и не только: Переделать. Ресайз должен жить в ConEmuChild!");
		//	GetWindowRect(hWndDC, &rcCurCon);
		//	MapWindowPoints(nullptr, ghWnd, (LPPOINT)&rcCurCon, 2);
		//	// Тут нас интересует только X/Y
		//	lbPosChanged = memcmp(&rcCurCon, &rcNewCon, sizeof(POINT))!=0;

		//	if (lbPosChanged)
		//	{
		//		// Двигаем окошко DC
		//		SetWindowPos(hWndDC, nullptr, rcNewCon.left, rcNewCon.top, 0,0, SWP_NOSIZE|SWP_NOZORDER);
		//	}
		//}
	}

	// Двигаем/ресайзим окошко DC
	pGroup->RepositionVCon(rcNewCon, true);

	//HWND hWndDC = pVConCurrent ? pVConCurrent->GetView() : nullptr;
	//if (hWndDC)
	//{
	//	WARNING("DoubleView и не только: Переделать. Ресайз должен жить в ConEmuChild!");
	//	GetWindowRect(hWndDC, &rcCurCon);
	//	MapWindowPoints(nullptr, ghWnd, (LPPOINT)&rcCurCon, 2);
	//	lbPosChanged = memcmp(&rcCurCon, &rcNewCon, sizeof(RECT))!=0;

	//	if (lbPosChanged)
	//	{
	//		// Двигаем/ресайзим окошко DC
	//		MoveWindow(hWndDC, rcNewCon.left, rcNewCon.top, rcNewCon.right - rcNewCon.left, rcNewCon.bottom - rcNewCon.top, 1);
	//		pVConCurrent->Invalidate();
	//	}
	//}
}

bool CVConGroup::isValid(CRealConsole* apRCon)
{
	if (!apRCon)
		return false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (VCon.VCon() && apRCon == VCon.VCon()->RCon())
			return true;
	}

	return false;
}

bool CVConGroup::isValid(CVirtualConsole* apVCon)
{
	if (!apVCon)
		return false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (apVCon == gp_VCon[i])
			return true;
	}

	return false;
}

bool CVConGroup::setRef(CVirtualConsole*& rpRef, CVirtualConsole* apVCon)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	bool bValid = isValid(apVCon);

	if (apVCon && !bValid)
	{
		_ASSERTE(FALSE && "apVCon was destroyed before AddRef?");
		apVCon = nullptr;
	}

	if (rpRef != apVCon)
	{
		CVirtualConsole *pOldRef = rpRef;

		rpRef = apVCon;

		if (pOldRef != rpRef)
		{
			if (rpRef)
			{
				rpRef->AddRef();
			}

			lockGroups.Unlock();

			if (pOldRef)
			{
				pOldRef->Release();
			}
		}
		else
		{
			_ASSERTE(pOldRef != rpRef);
		}
	}

	return bValid;
}

void CVConGroup::CheckTabValid(CTabID* apTab, bool& rbVConValid, bool& rbPidValid, bool& rbPassive)
{
	bool bVConValid = false, bPidValid = false, bPassive = false;

	if (apTab)
	{
		DWORD nCurFarPid;
		CVirtualConsole* pVCon = (CVirtualConsole*)apTab->Info.pVCon;
		bVConValid = isValid(pVCon);

		if (bVConValid)
		{
			if (apTab->Info.nPID)
			{
				bPidValid = pVCon->RCon()->isProcessExist(apTab->Info.nPID);
				nCurFarPid = pVCon->RCon()->GetFarPID(true);
				bPassive = (nCurFarPid != apTab->Info.nPID);
			}
			else
			{
				_ASSERTE(FALSE && "Must be filled? What about simple consoles?");
				bPidValid = true;
			}
		}
	}

	rbVConValid = bVConValid;
	rbPidValid = bPidValid;
	rbPassive = bPassive;
}

// nIdx - zero-based index
bool CVConGroup::isVConExists(int nIdx)
{
	if (nIdx < 0 || nIdx >= (int)countof(gp_VCon))
		return false;
	return (gp_VCon[nIdx] != nullptr);
}

bool CVConGroup::isVConHWND(HWND hChild, CVConGuard* rpVCon /*= nullptr*/)
{
	CVConGuard VCon;

	if (hChild)
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			CVConGuard VConI(gp_VCon[i]);
			if (VConI.VCon() && (VConI->GetView() == hChild))
			{
				VCon.Attach(VConI.VCon());
				break;
			}
		}
	}

	if (rpVCon)
		rpVCon->Attach(VCon.VCon());

	bool bFound = (VCon.VCon() != nullptr);
	return bFound;
}

bool CVConGroup::isEditor()
{
	if (!gp_VActive) return false;

	CVConGuard VCon(gp_VActive);
	return VCon.VCon() && VCon->RCon()->isEditor();
}

bool CVConGroup::isViewer()
{
	if (!gp_VActive) return false;

	CVConGuard VCon(gp_VActive);
	return VCon.VCon() && VCon->RCon()->isViewer();
}

bool CVConGroup::isFar(bool abPluginRequired/*=false*/)
{
	if (!gp_VActive) return false;

	CVConGuard VCon(gp_VActive);
	return VCon.VCon() && VCon->RCon()->isFar(abPluginRequired);
}

// Если ли фар где-то?
// Return "1-based"(!) value. 1 - Far Panels (or console), 2,3,... - Far Editor/Viewer windows
int CVConGroup::isFarExist(CEFarWindowType anWindowType/*=fwt_Any*/, const wchar_t* asName/*=nullptr*/, CVConGuard* rpVCon/*=nullptr*/)
{
	int iFound = 0;
	bool lbLocked = false;
	CVConGuard VCon;

	if (rpVCon)
		rpVCon->Release();

	for (INT_PTR i = -1; !iFound && (i < (INT_PTR)countof(gp_VCon)); i++)
	{
		if (i == -1)
			VCon.Attach(gp_VActive);
		else
			VCon.Attach(gp_VCon[i]);

		if (VCon.VCon())
		{
			// Это фар?
			CRealConsole* pRCon = VCon->RCon();
			if (!pRCon)
				continue;

			if (pRCon && pRCon->isFar((anWindowType & fwt_PluginRequired)==fwt_PluginRequired))
			{
				// Ищем что-то конкретное?
				if (!(anWindowType & (fwt_TypeMask|fwt_Elevated|fwt_NonElevated|fwt_ModalFarWnd|fwt_NonModal|fwt_ActivateFound|fwt_ActivateOther)) && !(asName && *asName))
				{
					iFound = 1; // Just "exists"
					break;
				}

				if (!(anWindowType & (fwt_TypeMask|fwt_ActivateFound|fwt_ActivateOther)) && !(asName && *asName))
				{
					CEFarWindowType t = pRCon->GetActiveTabType();

					// Этот Far Elevated?
					if ((anWindowType & fwt_Elevated) && !(t & fwt_Elevated))
						continue;
					// В табе устанавливается флаг fwt_Elevated
					// fwt_NonElevated используется только как аргумент поиска
					if ((anWindowType & fwt_NonElevated) && (t & fwt_Elevated))
						continue;

					// Модальное окно?
					WARNING("Нужно еще учитывать <модальность> заблокированным диалогом, или меню, или еще чем-либо!");
					if ((anWindowType & fwt_ModalFarWnd) && !(t & fwt_ModalFarWnd))
						continue;
					// В табе устанавливается флаг fwt_Modal
					// fwt_NonModal используется только как аргумент поиска
					if ((anWindowType & fwt_NonModal) && (t & fwt_ModalFarWnd))
						continue;

					iFound = 1; // Just "exists"
					break;
				}
				else
				{
					// Нужны доп.проверки окон фара
					CTab tab(__FILE__,__LINE__);
					LPCWSTR pszNameOnly = (anWindowType & fwt_FarFullPathReq) ? nullptr : asName ? PointToName(asName) : nullptr;
					if (pszNameOnly)
					{
						// Обработаем как обратные (в PointToName), так и прямые слеши
						// Это может быть актуально при переходе на ошибку/гиперссылку
						LPCWSTR pszSlash = wcsrchr(pszNameOnly, L'/');
						if (pszSlash)
							pszNameOnly = pszSlash+1;
					}

					for (int J = 0; !iFound; J++)
					{
						if (!pRCon->GetTab(J, tab))
							break;
						int j = tab->Info.nFarWindowID;

						CEFarWindowType tabFlags = tab->Info.Type;
						if ((tabFlags & fwt_TypeMask) != (anWindowType & fwt_TypeMask))
							continue;

						// Этот Far Elevated?
						if ((anWindowType & fwt_Elevated) && !(tabFlags & fwt_Elevated))
							continue;
						// В табе устанавливается флаг fwt_Elevated
						// fwt_NonElevated используется только как аргумент поиска
						if ((anWindowType & fwt_NonElevated) && (tabFlags & fwt_Elevated))
							continue;

						// Модальное окно?
						WARNING("Нужно еще учитывать <модальность> заблокированным диалогом, или меню, или еще чем-либо!");
						if ((anWindowType & fwt_ModalFarWnd) && !(tabFlags & fwt_ModalFarWnd))
							continue;
						// В табе устанавливается флаг fwt_Modal
						// fwt_NonModal используется только как аргумент поиска
						if ((anWindowType & fwt_NonModal) && (tabFlags & fwt_ModalFarWnd))
							continue;

						// Если ищем конкретный редактор/вьювер
						if (asName && *asName)
						{
							// Note. Для панелей - тут пустая строка
							const wchar_t* tabName = tab->Name.Ptr();
							if (lstrcmpi(tabName, asName) == 0)
							{
								iFound = (j+1);
							}
							else if (pszNameOnly && (pszNameOnly != asName)
								&& (lstrcmpi(PointToName(tabName), pszNameOnly) == 0))
							{
								iFound = (j+1);
							}
						}
						else
						{
							iFound = (j+1);
						}


						if (iFound)
						{
							if (anWindowType & (fwt_ActivateFound|fwt_ActivateOther))
							{
								bool bNeedActivateWnd = (anWindowType & fwt_ActivateFound)
									|| ((anWindowType & fwt_ActivateOther) && !pRCon->isActive(false));

								if (bNeedActivateWnd && !pRCon->ActivateFarWindow(j))
								{
									lbLocked = true;
								}

								if (!lbLocked && !pRCon->isActive(false))
								{
									gpConEmu->Activate(VCon.VCon());
								}
							}

							break;
						}
					}
				}
			}
		}
	}

	// Нашли?
	if (iFound)
	{
		if (rpVCon)
		{
			rpVCon->Attach(VCon.VCon());
			if (lbLocked)
				iFound = 0; // Failed (FALSE!)
		}
	}

	return iFound;
}

bool CVConGroup::EnumVCon(EnumVConFlags what, EnumVConProc pfn, LPARAM lParam)
{
	if (what < evf_Active || what > evf_All || pfn == nullptr)
		return false;

	bool bProcessed = false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (!VCon.Attach(gp_VCon[i]))
			continue;

		switch (what)
		{
		case evf_Visible:
			if (!VCon->isVisible())
				continue;
			break;
		case evf_Active:
			if (!VCon->isActive(false))
				continue;
			break;
		case evf_GroupSet:
			if (VCon->isGroupedInput() != evf_GroupSet)
				continue;
			break;
		}

		// And call the callback
		bProcessed = true;
		if (!pfn(VCon.VCon(), lParam))
			break;
	}

	return bProcessed;
}

// Возвращает индекс (0-based) активной консоли
int CVConGroup::GetActiveVCon(CVConGuard* pVCon /*= nullptr*/, int* pAllCount /*= nullptr*/)
{
	int nCount = 0, nFound = -1;
	CVConGuard VCon;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			nCount++;

			if (VCon.VCon() == gp_VActive)
			{
				if (pVCon)
					pVCon->Attach(VCon.VCon());
				nFound = i;
				// Continue enumeration if we need to fill pAllCount
				if (!pAllCount)
					break;
			}
		}
	}

	_ASSERTE((gp_VActive!=nullptr) == (nFound>=0));

	if (pAllCount)
		*pAllCount = nCount;

	return nFound;
}

// Возвращает индекс (0-based) консоли, или -1, если такой нет
int CVConGroup::GetVConIndex(CVirtualConsole* apVCon)
{
	if (!apVCon)
		return -1;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == apVCon)
			return i;
	}

	return -1;
}

bool CVConGroup::GetProgressInfo(short& pnProgress, bool& pbActiveHasProgress, AnsiProgressStatus& state)
{
	short nProgress = -1;
	const short nUpdateProgress = gpUpd ? gpUpd->GetUpdateProgress() : -1;
	// ReSharper disable once CppJoinDeclarationAndAssignment
	short n = -1;
	bool bActiveHasProgress = FALSE;
	bool wasError = FALSE;
	bool wasIndeterminate = FALSE;
	bool wasPaused = false;
	AnsiProgressStatus nState{};

	CVConGuard VCon;

	if (VCon.Attach(gp_VActive))
	{
		bool lbNotFromTitle = false;
		if (!isValid(VCon.VCon()))
		{
			_ASSERTE(isValid(VCon.VCon()));
		}
		else if ((nProgress = VCon->RCon()->GetProgress(&nState, &lbNotFromTitle)) >= 0)
		{
			switch (nState)
			{
			case AnsiProgressStatus::Error:
				wasError = true; break;
			case AnsiProgressStatus::Indeterminate:
				wasIndeterminate = true; break;
			case AnsiProgressStatus::Paused:
				wasPaused = true; break;
			default:
				break;
			}
			bActiveHasProgress = TRUE;
			_ASSERTE(lbNotFromTitle==FALSE); // CRealConsole должен проценты добавлять в GetTitle сам
		}
	}

	if (!bActiveHasProgress && nUpdateProgress >= 0)
	{
		if (nUpdateProgress <= UPD_PROGRESS_DOWNLOAD_START)
			wasIndeterminate = TRUE;
		nProgress = std::max(nProgress, nUpdateProgress);
	}

	// нас интересует возможное наличие ошибки во всех остальных консолях
	for (auto& iterCon : gp_VCon)
	{
		if (VCon.Attach(iterCon))
		{
			AnsiProgressStatus nCurState{};
			n = VCon->RCon()->GetProgress(&nCurState);

			switch (nCurState)
			{
			case AnsiProgressStatus::Error:
				wasError = true; break;
			case AnsiProgressStatus::Indeterminate:
				wasIndeterminate = true; break;
			case AnsiProgressStatus::Paused:
				wasPaused = true; break;
			default:
				break;
			}

			if (!bActiveHasProgress && n > nProgress)
				nProgress = n;
		}
	}

	pnProgress = nProgress;
	pbActiveHasProgress = bActiveHasProgress;
	if (wasError)
		state = AnsiProgressStatus::Error;
	else if (wasPaused)
		state = AnsiProgressStatus::Paused;
	else if (wasIndeterminate)
		state = AnsiProgressStatus::Indeterminate;
	else if (nProgress >= 0)
		state = AnsiProgressStatus::Running;
	else
		state = AnsiProgressStatus::None;

	return (nProgress >= 0);
}

void CVConGroup::OnDosAppStartStop(HWND hwnd, StartStopType sst, DWORD idChild)
{
	CVConGuard VCon;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (!VCon.Attach(gp_VCon[i])) continue;

		// Запускаемые через "-new_console" цепляются через CECMD_ATTACH2GUI, а не через WinEvent
		// 111211 - "-new_console" теперь передается в GUI и исполняется в нем
		if (VCon->RCon()->isDetached() || !VCon->RCon()->isServerCreated())
			continue;

		HWND hRConWnd = VCon->RCon()->ConWnd();
		if (hRConWnd == hwnd)
		{
			//StartStopType sst = (anEvent == EVENT_CONSOLE_START_APPLICATION) ? sst_App16Start : sst_App16Stop;
			VCon->RCon()->OnDosAppStartStop(sst, idChild);
			break;
		}
	}
}

void CVConGroup::InvalidateAll()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{
			VCon.VCon()->Invalidate();
		}
	}
}

void CVConGroup::UpdateWindowChild(CVirtualConsole* apVCon)
{
	CVConGuard VCon;
	if (apVCon)
	{
		if (VCon.Attach(apVCon)
			&& VCon->isVisible())
		{
			UpdateWindow(VCon->GetView());
		}
	}
	else
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
				UpdateWindow(VCon->GetView());
		}
	}
}

#if 0
void CVConGroup::RePaint()
{
	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{
			HWND hView = VCon->GetView();
			if (hView)
			{
				HDC hDc = GetDC(hView);
				//RECT rcClient = pVCon->GetDcClientRect();
				VCon->PaintVCon(hDc/*, rcClient*/);
				ReleaseDC(ghWnd, hDc);
			}
		}
	}
}
#endif

void CVConGroup::Update(bool isForce /*= false*/)
{
	CVConGuard VCon;

	if (isForce)
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			if (VCon.Attach(gp_VCon[i]))
				VCon->OnFontChanged();
		}
	}

	CVirtualConsole::ClearPartBrushes();

	if (VCon.Attach(gp_VActive))
	{
		VCon->Update(isForce);
		//InvalidateAll();
	}
}

// 2del???
bool CVConGroup::isActiveGroupVCon(CVirtualConsole* pVCon)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	if (!isValid(pVCon))
		return false;

	CVConGroup* pGr = GetRootOfVCon(pVCon);
	if (!pGr)
		return false;

	if (pGr->mp_ActiveGroupVConPtr == (void*)pVCon)
		return true;

	// Та, что была ранее активной - могла закрыться
	if (!isValid((CVirtualConsole*)pGr->mp_ActiveGroupVConPtr))
	{
		// Тогда ставим первой - эту консоль!
		pGr->StoreActiveVCon(pVCon);
		return true;
	}

	return false;
}

bool CVConGroup::isConSelectMode()
{
	CVConGuard VCon;

	//TODO: По курсору, что-ли попробовать определять?
	//return gb_ConsoleSelectMode;
	if (VCon.Attach(gp_VActive))
		return VCon->RCon()->isConSelectMode();

	return false;
}

bool CVConGroup::isInCreateRoot()
{
	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			if (VCon->RCon()->InCreateRoot())
				return true;
		}
	}
	return false;
}

bool CVConGroup::isDetached()
{
	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			if (VCon->RCon()->isDetached())
				return true;
		}
	}
	return false;
}

bool CVConGroup::isFilePanel(bool abPluginAllowed/*=false*/)
{
	if (!gp_VActive) return false;

	CVConGuard VCon(gp_VActive);
	bool lbIsPanels = VCon.VCon() && VCon->RCon()->isFilePanel(abPluginAllowed);
	return lbIsPanels;
}

bool CVConGroup::isNtvdm(BOOL abCheckAllConsoles/*=FALSE*/)
{
	CVConGuard VCon;

	if (VCon.Attach(gp_VActive))
	{
		CRealConsole* pRCon = VCon->RCon();

		if (pRCon && pRCon->isNtvdm())
			return true;
	}

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		// Let's check all console, even gp_VActive (it may be changed)
		if (!VCon.Attach(gp_VCon[i]))
			continue;
		CRealConsole* pRCon = VCon->RCon();
		if (!pRCon)
			continue;

		if (pRCon->isNtvdm())
			return true;
	}

	return false;
}

bool CVConGroup::isOurConsoleWindow(HWND hCon)
{
	if (!hCon)
		return false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
		if (!pRCon)
			continue;

		if (pRCon->ConWnd() == hCon)
			return true;
	}

	return false;
}

// Вернуть true, если hAnyWnd это наше окно, консольное окно,
// или принадлежит дочернему GUI приложению, запущенному во вкладке ConEmu
bool CVConGroup::isOurWindow(HWND hAnyWnd)
{
	if (!hAnyWnd)
		return false;

	// ConEmu/Settings?
	if ((hAnyWnd == ghWnd) || (hAnyWnd == ghOpWnd))
		return true;

	DWORD nPID = 0;
	if (!GetWindowThreadProcessId(hAnyWnd, &nPID))
		return false;
	// Еще какие-то наши окна?
	if (nPID == GetCurrentProcessId())
		return true;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
		if (!pRCon)
			continue;

		if (pRCon->ConWnd() == hAnyWnd)
			return true;

		if ((pRCon->GuiWnd() == hAnyWnd) || (pRCon->GuiWndPID() == nPID))
			return true;
	}

	return false;
}

bool CVConGroup::isOurGuiChildWindow(HWND hWnd)
{
	if (!hWnd)
		return false;

	DWORD nPID = 0;
	if (!GetWindowThreadProcessId(hWnd, &nPID))
		return false;
	// интересует только Child GUI
	if (nPID == GetCurrentProcessId())
		return false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
		if (!pRCon)
			continue;

		if ((pRCon->GuiWnd() == hWnd) || (pRCon->GuiWndPID() == nPID))
			return true;
	}

	return false;
}

bool CVConGroup::isChildWindowVisible()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{
			CRealConsole* pRCon = VCon->RCon();
			if (!pRCon)
				continue;

			if (pRCon->GuiWnd() || pRCon->isPictureView())
				return true;
		}
	}

	return false;
}

// Заголовок окна для PictureView вообще может пользователем настраиваться, так что
// рассчитывать на него при определения "Просмотра" - нельзя
bool CVConGroup::isPictureView()
{
	bool lbRc = false;

	if (gpConEmu->hPictureView && (!IsWindow(gpConEmu->hPictureView) || !isFar()))
	{
		InvalidateAll();
		gpConEmu->hPictureView = nullptr;
	}

	bool lbPrevPicView = (gpConEmu->hPictureView != nullptr);

	HWND hPicView = nullptr;

	for (size_t i = 0; !lbRc && i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (!VCon.Attach(gp_VCon[i]))
			continue;

		// Было isVisible, но некорректно блокировать другие сплиты, в которых PicView нету
		if (!VCon->isActive(false) || !VCon->RCon())
			continue;

		hPicView = VCon->RCon()->isPictureView();

		lbRc = (hPicView != nullptr);

		// Если вызывали Help (F1) - окошко PictureView прячется
		if (hPicView && !IsWindowVisible(hPicView))
		{
			lbRc = false;
			hPicView = nullptr;
		}
	}

	gpConEmu->hPictureView = hPicView;

	if (gpConEmu->bPicViewSlideShow && !gpConEmu->hPictureView)
	{
		gpConEmu->bPicViewSlideShow = false;
	}

	if (lbRc && !lbPrevPicView)
	{
		GetWindowRect(ghWnd, &gpConEmu->mrc_WndPosOnPicView);
	}
	else if (!lbRc)
	{
		memset(&gpConEmu->mrc_WndPosOnPicView, 0, sizeof(gpConEmu->mrc_WndPosOnPicView));
	}

	return lbRc;
}

void CVConGroup::OnRConTimerCheck()
{
	EnumVCon(evf_Visible, [](CVirtualConsole* pVCon, LPARAM lParam)
		{
			auto* pRCon = pVCon->RCon();
			if (pRCon)
			{
				pRCon->OnTimerCheck();
			}
			return true;
		}, 0);
}

void CVConGroup::OnRConSelectionTimerCheck()
{
	EnumVCon(evf_Visible, [](CVirtualConsole* pVCon, LPARAM lParam)
		{
			auto* pRCon = pVCon->RCon();
			if (pRCon)
			{
				pRCon->OnSelectionTimerCheck();
			}
			return true;
		}, 0);
}

// nIdx - 0 based
// bFromCycle = true, для перебора в циклах (например, в CSettings::UpdateWinHookSettings), чтобы не вылезали ассерты
bool CVConGroup::GetVCon(int nIdx, CVConGuard* pVCon /*= nullptr*/, bool bFromCycle /*= false*/)
{
	bool bFound = false;

	if (nIdx < 0 || nIdx >= (int)countof(gp_VCon) || gp_VCon[nIdx] == nullptr)
	{
		_ASSERTE((nIdx>=0) && (nIdx<(int)MAX_CONSOLE_COUNT || (bFromCycle && nIdx==(int)MAX_CONSOLE_COUNT)));
		if (pVCon)
			pVCon->Release();
	}
	else
	{
		if (pVCon)
			pVCon->Attach(gp_VCon[nIdx]);
		bFound = true;
	}

	return bFound;
}

bool CVConGroup::GetVConFromPoint(POINT ptScreen, CVConGuard* pVCon /*= nullptr*/)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	bool bFound = false;
	CVConGuard VCon;

#if 0
	if ((GetActiveVCon(&VCon) >= 0) && VCon->RCon()->isMouseSelectionPresent())
	{
		if (pVCon)
			pVCon->Attach(VCon.VCon());
		return true;
	}
#endif

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{

			HWND hView = VCon->GetView();
			HWND hBack = VCon->GetBack();
			if (hView && hBack)
			{
				// Check hBack, because it is larger and ScrollBar may lie outside of hView
				RECT rcView; GetWindowRect(hBack, &rcView);

				if (PtInRect(&rcView, ptScreen))
				{
					if (pVCon)
						pVCon->Attach(VCon.VCon());
					bFound = true;
					break;
				}
			}
			#ifdef _DEBUG
			else
			{
				lockGroups.Unlock();
				_ASSERTE((hView && hBack) && "(hView = VCon->GetView()) != nullptr");
				lockGroups.Lock(gpcs_VGroups);
			}
			#endif
		}
	}

	return bFound;
}

//bool CVConGroup::ResizeConsoles(const RECT &rFrom, enum ConEmuRect tFrom)
//{
//	_ASSERTE(FALSE && "TODO");
//	return false;
//}
//
//bool CVConGroup::ResizeViews(bool bResizeRCon/*=true*/, WPARAM wParam/*=0*/, WORD newClientWidth/*=(WORD)-1*/, WORD newClientHeight/*=(WORD)-1*/)
//{
//	_ASSERTE(FALSE && "TODO");
//	return false;
//}

bool CVConGroup::CloseQuery(MArray<CVConGuard*>* rpPanes, bool* rbMsgConfirmed /*= nullptr*/, bool bForceKill /*= false*/, bool bNoGroup /*= false*/)
{
	// "ConEmu.exe /NoCloseConfirm" --> silent mode...
	if (gpConEmu->DisableCloseConfirm)
	{
		if (rbMsgConfirmed)
			*rbMsgConfirmed = true;
		return true;
	}

	CVConGuard VCon(gp_VActive);

	int nEditors = 0, nProgress = 0, i, nConsoles = 0;
	if (rbMsgConfirmed)
		*rbMsgConfirmed = false;

	if (rpPanes)
		i = (int)rpPanes->size() - 1;
	else
		i = (int)countof(gp_VCon) - 1;

	for (; i >= 0; --i)
	{
		CRealConsole* pRCon = nullptr;
		CVConGuard iVCon;
		CVirtualConsole* pVCon;

		if (rpPanes)
		{
			if (!(pVCon = (*rpPanes)[i]->VCon()))
				continue;
		}
		else
		{
			if (!GetVCon(i, &iVCon, true))
				continue;
			pVCon = iVCon.VCon();
		}

		if (pVCon && ((pRCon = pVCon->RCon()) != nullptr))
		{
			nConsoles++;

			// Прогрессы (копирование, удаление, и т.п.)
			if (pRCon->GetProgress(nullptr) != -1)
				nProgress ++;
			else if ((gpConEmu->CloseConfirmFlags() & Settings::cc_Running) && pRCon->GetRunningPID())
				nProgress ++;

			// Несохраненные редакторы
			int n = pRCon->GetModifiedEditors();

			if (n)
				nEditors += n;
		}
	}

	bool bCloseConfirmSet = (gpConEmu->CloseConfirmFlags() & Settings::cc_Window) != 0;
	if (bCloseConfirmSet && gpUpd)
	{
		// Если только что был запрос на обновление-и-закрытие, то нет смысла подтверждать закрытие
		CConEmuUpdate::UpdateStep step = gpUpd->InUpdate();
		if (step == CConEmuUpdate::UpdateStep::ExitAndUpdate)
		{
			bCloseConfirmSet = false;
		}
	}

	if (nProgress || nEditors || (bCloseConfirmSet && (nConsoles >= 1)))
	{
		int nBtn = IDCANCEL;

		// if we are closing group of panes
		if (rpPanes)
		{
			// Use TaskDialog?
			if (gOSVer.dwMajorVersion >= 6)
			{
				ConfirmCloseParam Parm;
				Parm.bGroup = bNoGroup ? ConfirmCloseParam::eNoGroup : ConfirmCloseParam::eGroup;
				Parm.bForceKill = bForceKill;
				Parm.nConsoles = nConsoles;
				Parm.nOperations = nProgress;
				Parm.nUnsavedEditors = nEditors;

				nBtn = ConfirmCloseConsoles(Parm);
			}
			else
			{
				TODO("Перенести в ConfirmDlg.cpp");

				wchar_t szText[360], *pszText;
				//wcscpy_c(szText, L"Close confirmation.\r\n\r\n");
				swprintf_c(szText, L"About to close %u console%s.\r\n", nConsoles, (nConsoles>1)?L"s":L"");
				pszText = szText+_tcslen(szText);

				if (nProgress || nEditors)
				{
					*(pszText++) = L'\r'; *(pszText++) = L'\n'; *(pszText) = 0;

					if (nProgress)
					{
						swprintf_c(pszText, countof(szText)-(pszText-szText)/*#SECURELEN*/, L"Incomplete operations: %i\r\n", nProgress);
						pszText += _tcslen(pszText);
					}
					if (nEditors)
					{
						swprintf_c(pszText, countof(szText)-(pszText-szText)/*#SECURELEN*/, L"Unsaved editor windows: %i\r\n", nEditors);
						pszText += _tcslen(pszText);
					}
				}

				wcscat_c(szText, L"\r\nProceed with close group?");

				nBtn = MsgBox(szText, MB_OKCANCEL|MB_ICONEXCLAMATION, gpConEmu->GetDefaultTitle(), ghWnd);
			}
		}
		else if ((nConsoles == 1) && (gpConEmu->CloseConfirmFlags() & Settings::cc_Console) && VCon.VCon())
		{
			if (VCon->RCon()->isCloseTabConfirmed(fwt_Panels, nullptr, true))
			{
				nBtn = IDOK;
			}
		}
		else
		{
			ConfirmCloseParam Parm;
			Parm.bForceKill = bForceKill;
			Parm.nConsoles = nConsoles;
			Parm.nOperations = nProgress;
			Parm.nUnsavedEditors = nEditors;

			nBtn = ConfirmCloseConsoles(Parm);
		}

		if (nBtn == IDNO)
		{
			if (VCon.VCon())
			{
				VCon->RCon()->CloseConsole(false, false);
			}
			return false; // Don't close others!
		}
		else if ((nBtn != IDYES) && (nBtn != IDOK))
		{
			return false; // не закрывать
		}

		if (rbMsgConfirmed)
			*rbMsgConfirmed = true;
	}

	return true; // можно
}

bool CVConGroup::OnCloseQuery(bool* rbMsgConfirmed /*= nullptr*/)
{
	if (!CloseQuery(nullptr, rbMsgConfirmed))
	{
		gpConEmu->LogString(L"CloseQuery blocks closing");
		gpConEmu->SetScClosePending(false);
		return false;
	}

	// Выставить флажок, чтобы ConEmu знал: "закрытие инициировано пользователем через крестик/меню"
	gpConEmu->SetScClosePending(true);

	#ifdef _DEBUG
	if (gnInMyAssertTrap > 0)
	{
		gpConEmu->LogString(L"CloseQuery skipped due to gnInMyAssertTrap");
		return false;
	}
	#endif

	// Чтобы мог сработать таймер закрытия
	gpConEmu->OnRConStartedSuccess(nullptr);
	return true; // можно
}

// true - found
bool CVConGroup::OnFlashWindow(DWORD nOpt, DWORD nFlags, DWORD nCount, HWND hCon)
{
	if (!hCon) return false;

	wchar_t szLog[120];
	swprintf_c(szLog, L"OnFlashWindow: opt=x%02X flags=x%02X count=%u con=x%08X", nOpt, nFlags, nCount, LODWORD(hCon));
	LogString(szLog);

	const bool abSimple = (nOpt & 1) != 0;
	const bool abInvert = (nOpt & 2) != 0;
	const bool abFromMacro = (nOpt & 4) != 0;
	const bool abFromApi = (nOpt & 8) != 0;

	bool lbFlashSimple = abSimple;

	HWND hWndFlash = gpConEmu->GetRootHWND();

	if (abFromMacro)
	{
		// From GuiMacro - anything is allowed
	}
	// Option to disable it completely
	else if (gpSet->isDisableFarFlashing && gp_VActive->RCon()->GetFarPID(FALSE))
	{
		if (gpSet->isDisableFarFlashing == 1)
			return false;
		else
			lbFlashSimple = true;
	}
	else if (gpSet->isDisableAllFlashing)
	{
		if (gpSet->isDisableAllFlashing == 1)
			return false;
		else
			lbFlashSimple = true;
	}

	bool lbFound = false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (!gp_VCon[i]) continue;

		if (gp_VCon[i]->RCon()->ConWnd() == hCon)
		{
			lbFound = true;

			FLASHWINFO fl = {sizeof(FLASHWINFO)};

			if (abFromMacro || abFromApi)
			{
				// From GuiMacro - anything is allowed
				if (abSimple)
				{
					FlashWindow(hWndFlash, abInvert);
				}
				else
				{
					fl.dwFlags = FLASHW_STOP; fl.hwnd = hWndFlash;
					FlashWindowEx(&fl); // Don't accumulate flashing
					fl.uCount = nCount; fl.dwFlags = nFlags; fl.hwnd = hWndFlash;
					FlashWindowEx(&fl);
				}
			}
			else if (gpConEmu->isMeForeground())
			{
				if (gp_VCon[i] != gp_VActive)    // For inactive VCon-s only
				{
					fl.dwFlags = FLASHW_STOP; fl.hwnd = hWndFlash;
					FlashWindowEx(&fl); // Don't accumulate flashing
					fl.uCount = 3; fl.dwFlags = lbFlashSimple ? FLASHW_ALL : FLASHW_TRAY; fl.hwnd = hWndFlash;
					FlashWindowEx(&fl);
				}
			}
			else
			{
				if (lbFlashSimple)
				{
					fl.uCount = 3; fl.dwFlags = FLASHW_TRAY;
				}
				else
				{
					fl.dwFlags = FLASHW_ALL|FLASHW_TIMERNOFG;
				}

				fl.hwnd = hWndFlash;
				FlashWindowEx(&fl); // Помигать в GUI
			}

			break;
		}
	}

	return lbFound;
}

void CVConGroup::ExportEnvVarAll(CESERVER_REQ* pIn, CRealConsole* pExceptRCon)
{
	// Просто перебить заголовок на наши данные
	ExecutePrepareCmd(&pIn->hdr, CECMD_EXPORTVARS, pIn->hdr.cbSize);

	// и пробежаться по табам
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (VCon.VCon() == nullptr)
			continue;
		CRealConsole* pRCon = VCon->RCon();
		if (pRCon == pExceptRCon)
			continue;
		DWORD nSrvPID = pRCon->GetServerPID(true);
		if (!nSrvPID)
			continue;

		ConProcess* pP = nullptr;
		int nCount = pRCon->GetProcesses(&pP);
		if (nCount && pP)
		{
			// Apply to all processes in this tab (console)
			for (int i = 0; i < nCount; i++)
			{
				if (pP[i].ProcessID != nSrvPID && !pP[i].IsConHost)
				{
					CESERVER_REQ* pOut = ExecuteHkCmd(pP[i].ProcessID, pIn, ghWnd);
					ExecuteFreeResult(pOut);
				}
			}
			free(pP);
		}
	}
}

void CVConGroup::OnUpdateGuiInfoMapping(ConEmuGuiMapping* apGuiInfo)
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->RCon())
		{
			VCon->RCon()->UpdateGuiInfoMapping(apGuiInfo);
		}
	}
}

void CVConGroup::OnPanelViewSettingsChanged()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i])
		{
			gp_VCon[i]->OnPanelViewSettingsChanged();
		}
	}
}

void CVConGroup::OnTaskbarSettingsChanged()
{
	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
			VCon->OnTaskbarSettingsChanged();
	}
}

void CVConGroup::OnTaskbarCreated()
{
	CVConGuard VCon;
	HWND hGhost = nullptr;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			hGhost = VCon->GhostWnd();
			if (hGhost)
			{
				gpConEmu->OnGhostCreated(VCon.VCon(), hGhost);
			}
		}
	}
}

// true - если nPID запущен в одной из консолей
bool CVConGroup::isConsolePID(DWORD nPID)
{
	bool lbPidFound = false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == nullptr || gp_VCon[i]->RCon() == nullptr)
			continue;

		DWORD dwFarPID    = gp_VCon[i]->RCon()->GetFarPID();
		DWORD dwActivePID = gp_VCon[i]->RCon()->GetActivePID();

		if (dwFarPID == nPID || dwActivePID == nPID)
		{
			lbPidFound = true;
			break;
		}
	}

	return lbPidFound;
}

// returns true if gpConEmu->Destroy() was called
// bMsgConfirmed - был ли показан диалог подтверждения, или юзер не включил эту опцию
bool CVConGroup::DoCloseAllVCon(bool bMsgConfirmed)
{
	gpConEmu->LogString(L"CVConGroup::OnScClose()");

	bool lbAllowed = false;
	int nConCount = 0, nDetachedCount = 0;
	bool lbProceed = false;

	bool bConfirmEach = (bMsgConfirmed || !(gpConEmu->CloseConfirmFlags() & Settings::cc_Window)) ? false : true;

	// Сохраним размер перед закрытием консолей, а то они могут напакостить и "вернуть" старый размер
	gpSet->SaveSettingsOnExit();

	for (int i = (int)(countof(gp_VCon)-1); i >= 0; i--)
	{
		if (gp_VCon[i] && gp_VCon[i]->RCon())
		{
			if (gp_VCon[i]->RCon()->isDetached())
			{
				nDetachedCount ++;
				continue;
			}

			nConCount ++;

			if (gp_VCon[i]->RCon()->ConWnd())
			{
				gp_VCon[i]->RCon()->CloseConsole(false, bConfirmEach);
			}
		}
	}

	if (nConCount == 0)
	{
		if (nDetachedCount > 0)
		{
			if (MsgBox(L"ConEmu is waiting for console attach.\nIt was started in 'Detached' mode.\nDo You want to cancel waiting?",
			              MB_YESNO|MB_ICONQUESTION, gpConEmu->GetDefaultTitle(), ghWnd) == IDYES)
			{
				for (int i = (int)(countof(gp_VCon)-1); i >= 0; i--)
				{
					if (gp_VCon[i] && gp_VCon[i]->RCon())
					{
						if (gp_VCon[i]->RCon()->isDetached())
							gp_VCon[i]->RCon()->StopSignal();
					}
				}
				lbAllowed = true;
				nDetachedCount = 0;
			}
		}
		else
		{
			lbAllowed = true;
		}
	}

	bool bEmpty = (!nConCount && !nDetachedCount);

	// Закрыть окно, если просили
	if (lbAllowed && bEmpty && gpConEmu->isDestroyOnClose(bEmpty))
	{
		// Поэтому проверяем, и если никого не осталось, то по крестику - прибиваемся
		gpConEmu->Destroy();
	}
	else
	{
		lbAllowed = false;
	}

	return lbAllowed;
}

void CVConGroup::CloseAllButActive(CVirtualConsole* apVCon/*may be null*/, CloseConsoleMode closeMode, bool abNoConfirm)
{
	int i;
	MArray<CVConGuard*> VCons;
	CVConGuard* pGuard;
	bool active_found = false;

	for (i = (int)(countof(gp_VCon)-1); i >= 0; i--)
	{
		if (gp_VCon[i] == nullptr)
			continue;
		if (gp_VCon[i] == apVCon)
		{
			active_found = true;
			continue;
		}
		if (closeMode == CloseZombie)
		{
			if (gp_VCon[i]->RCon()->GetActivePID() != 0)
				continue;
		}
		else if (closeMode == Close2Right)
		{
			// we loop from the end to the beginning!
			if (active_found)
				break;
		}

		pGuard = new CVConGuard(gp_VCon[i]);
		VCons.push_back(pGuard);
	}

	if (abNoConfirm || CloseQuery(&VCons, nullptr, false, true))
	{
		gpConEmu->SetScClosePending(true); // Disable confirmation of each console closing

		for (i = 0; i < VCons.size(); i++)
		{
			pGuard = VCons[i];
			(*pGuard)->RCon()->CloseTab();
		}

		gpConEmu->SetScClosePending(false);
	}

	while (VCons.pop_back(pGuard))
	{
		SafeDelete(pGuard);
	}
}

void CVConGroup::CloseGroup(CVirtualConsole* apVCon/*may be null*/, bool abKillActiveProcess /*= false*/)
{
	CVConGuard VCon(gp_VActive);
	if (!gp_VActive)
		return;

	CVConGroup* pActiveGroup = GetRootOfVCon(apVCon ? apVCon : gp_VActive);
	if (!pActiveGroup)
		return;

	MArray<CVConGuard*> Panes;
	int nCount = pActiveGroup->GetGroupPanes(&Panes);
	if (nCount > 0)
	{
		if (CloseQuery(&Panes, nullptr, abKillActiveProcess))
		{
			for (int i = (nCount - 1); i >= 0; i--)
			{
				CVirtualConsole* pVCon = Panes[i]->VCon();
				if (pVCon)
				{
					pVCon->RCon()->CloseConsole(abKillActiveProcess, false);
				}
			}
		}
	}

	FreePanesArray(Panes);
}

void CVConGroup::OnUpdateScrollInfo()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (!VCon.VCon())
			continue;
		if (!VCon->isVisible())
			continue;

		if (VCon->RCon())
			VCon->RCon()->UpdateScrollInfo();
	}
}

// Послать во все активные фары CMD_FARSETCHANGED
// Обновляются настройки: gpSet->isFARuseASCIIsort, gpSet->isShellNoZoneCheck;
void CVConGroup::OnUpdateFarSettings()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (!VCon.VCon())
			continue;

		CRealConsole* pRCon = VCon->RCon();

		if (pRCon)
			pRCon->UpdateFarSettings();

		//DWORD dwFarPID = pRCon->GetFarPID();
		//if (!dwFarPID) continue;
		//pRCon->EnableComSpec(dwFarPID, gpSet->AutoBufferHeight);
	}
}

void CVConGroup::OnUpdateTextColorSettings(bool ChangeTextAttr /*= TRUE*/, bool ChangePopupAttr /*= TRUE*/, const AppSettings* apDistinct /*= nullptr*/)
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (!VCon.VCon())
			continue;

		CRealConsole* pRCon = VCon->RCon();

		if (pRCon)
		{
			pRCon->UpdateTextColorSettings(ChangeTextAttr, ChangePopupAttr, apDistinct);
		}
	}
}

void CVConGroup::OnVConClosed(CVirtualConsole* apVCon)
{
	bool bDbg1 = false, bDbg2 = false, bDbg3 = false, bDbg4 = false;
	int iDbg1 = -100, iDbg2 = -100, iDbg3 = -100;
	CVConGroup* pClosedGroup = nullptr;
	bool bInvalidVCon = false;

	if (!isValid(apVCon) || apVCon->isAlreadyDestroyed())
	{
		ShutdownGuiStep(L"OnVConClosed - was already closed");
		bInvalidVCon = true;
		goto wrap;
	}

	ShutdownGuiStep(L"OnVConClosed");

	//CVConGroup* pClosedGroupRoot = GetRootOfVCon(apVCon);
	pClosedGroup = ((CVConGroup*)apVCon->mp_Group);

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == apVCon)
		{
			iDbg1 = i;

			// Сначала нужно обновить закладки, иначе в закрываемой консоли
			// может быть несколько вкладок и вместо активации другой консоли
			// будет попытка активировать другую вкладку закрываемой консоли
			//gpConEmu->mp_TabBar->Update(TRUE); -- а и не сможет он другую активировать, т.к. RCon вернет FALSE

			if (apVCon == gp_VActive)
			{
				gpConEmu->mp_TabBar->SwitchRollback();
				if (gp_VCon[1] == nullptr)
				{
					// if there is only one console left - no chance to "change" it...
					_ASSERTE(GetConCount() <= 1);
				}
				// Firstly, try to activate pane in the same group
				else if (!ActivateNextPane(apVCon))
				{
					if (gpSet->isTabRecent)
						gpConEmu->mp_TabBar->SwitchNext();
					else
						gpConEmu->mp_TabBar->SwitchPrev();
					gpConEmu->mp_TabBar->SwitchCommit();
				}
			}

			pClosedGroup->RemoveGroup();


			//bool bAllowRecent = true;

			//if (gp_GroupPostCloseActivate)
			//{
			//	if (isValid(gp_GroupPostCloseActivate))
			//	{
			//		if (Activate(gp_GroupPostCloseActivate))
			//		{
			//			bAllowRecent = false;
			//		}
			//	}
			//	gp_GroupPostCloseActivate = nullptr;
			//}

			//// Эта комбинация должна активировать предыдущую консоль (если активна текущая)
			//if (bAllowRecent && gpSet->isTabRecent && apVCon == gp_VActive)
			//{
			//	if (gpConEmu->GetVCon(1))
			//	{
			//		bDbg1 = true;
			//		gpConEmu->mp_TabBar->SwitchRollback();
			//		gpConEmu->mp_TabBar->SwitchNext();
			//		gpConEmu->mp_TabBar->SwitchCommit();
			//	}
			//}

			// Теперь можно очистить переменную массива
			gp_VCon[i] = nullptr;
			WARNING("Вообще-то это нужно бы в CriticalSection закрыть. Несколько консолей может одновременно закрыться");

			//if (gp_VActive == apVCon)
			//{
			//	bDbg2 = true;

			//	for (int j=(i-1); j>=0; j--)
			//	{
			//		if (gp_VCon[j])
			//		{
			//			iDbg2 = j;
			//			ConActivate(j);
			//			break;
			//		}
			//	}

			//	if (gp_VActive == apVCon)
			//	{
			//		bDbg3 = true;

			//		for (size_t j = (i+1); j < countof(gp_VCon); j++)
			//		{
			//			if (gp_VCon[j])
			//			{
			//				iDbg3 = j;
			//				ConActivate(j);
			//				break;
			//			}
			//		}
			//	}
			//}

			for (size_t j = (i+1); j < countof(gp_VCon); j++)
			{
				gp_VCon[j-1] = gp_VCon[j];
			}

			gp_VCon[countof(gp_VCon)-1] = nullptr;

			if (gp_VActive == apVCon)
			{
				bDbg4 = true;
				setActiveVConAndFlags(nullptr);
			}
			else
			{
				setActiveVConAndFlags(gp_VActive);
			}

			apVCon->DoDestroyDcWindow();
			apVCon->Release();
			break;
		}
	}

wrap: // Wrap to here, because gp_VActive may be invalid already and we need to check it
	if (gp_VActive == apVCon)
	{
		// Сюда вообще попадать не должны, но на всякий случай, сбрасываем gp_VActive
		// Сюда можем попасть если не удалось создать активную консоль (облом с паролем, например)
		_ASSERTE((gp_VActive == nullptr && gp_VCon[0] == nullptr) || gb_CreatingActive);

		CVirtualConsole* pNewActive = nullptr;
		for (size_t i = countof(gp_VCon); i--;)
		{
			if (gp_VCon[i] && gp_VCon[i] != apVCon)
			{
				pNewActive = gp_VCon[i];
				break;
			}
		}

		setActiveVConAndFlags(pNewActive);
	}

	if (gp_VActive)
	{
		ShowActiveGroup(gp_VActive);
	}

	// Теперь перетряхнуть заголовок (табы могут быть отключены и в заголовке отображается количество консолей)
	gpConEmu->UpdateTitle(); // сам перечитает
	//
	gpConEmu->mp_TabBar->Update(); // Иначе не будет обновлены закладки
	// А теперь можно обновить активную закладку
	int nActiveConNum = gpConEmu->ActiveConNum();
	gpConEmu->mp_TabBar->OnConsoleActivated(nActiveConNum/*, FALSE*/);
	// StatusBar
	CVConGuard VCon;
	gpConEmu->mp_Status->OnActiveVConChanged(nActiveConNum, (gpConEmu->GetActiveVCon(&VCon) >= 0) ? VCon->RCon() : nullptr);
}

void CVConGroup::OnUpdateProcessDisplay(HWND hInfo)
{
	if (!hInfo)
		return;

	SendDlgItemMessage(hInfo, lbProcesses, LB_RESETCONTENT, 0, 0);

	wchar_t temp[MAX_PATH];

	for (int j = 0; j < countof(gp_VCon); j++)
	{
		if (gp_VCon[j] == nullptr) continue;

		ConProcess* pPrc = nullptr;
		int nCount = gp_VCon[j]->RCon()->GetProcesses(&pPrc);

		if (pPrc && (nCount > 0))
		{
			for (int i=0; i<nCount; i++)
			{
				if (gp_VCon[j] == gp_VActive)
					wcscpy_c(temp, L"(*) ");
				else
					temp[0] = 0;

				int iUsed = lstrlen(temp);
				swprintf_c(temp+iUsed, MAX_PATH-iUsed/*#SECURELEN*/, L"[%i.%i] %s - PID:%i",
						 j+1, i, pPrc[i].Name, pPrc[i].ProcessID);
				if (hInfo)
					SendDlgItemMessage(hInfo, lbProcesses, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(temp));
			}

			SafeFree(pPrc);
		}
	}
}

// Возвращает HWND окна отрисовки
HWND CVConGroup::DoSrvCreated(const DWORD nServerPID, const HWND hWndCon, const DWORD dwKeybLayout, DWORD& t1, DWORD& t2, int& iFound, CESERVER_REQ_SRVSTARTSTOPRET& pRet)
{
	HWND hWndDC = nullptr;

	//gpConEmu->WinEventProc(nullptr, EVENT_CONSOLE_START_APPLICATION, hWndCon, (LONG)nServerPID, 0, 0, 0);
	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (!gp_VCon[i] || !VCon.Attach(gp_VCon[i]))
			continue;
		CRealConsole* pRCon = VCon->RCon();
		if (pRCon && pRCon->isServerCreated())
		{
			if (pRCon->GetServerPID() == nServerPID)
			{
				iFound = i;
				t1 = timeGetTime();

				hWndDC = pRCon->OnServerStarted(hWndCon, nServerPID, dwKeybLayout, pRet);

				t2 = timeGetTime();
				break;
			}
		}
	}

	return hWndDC;
}

CVConGroup* CVConGroup::FindNextPane(const RECT& rcPrev, int nHorz /*= 0*/, int nVert /*= 0*/) const
{
	CVConGroup* pNext = nullptr;
	int iMinDistance = 0;
	MArray<CVConGuard*> Panes;
	int nGroupPanes;
	int iPrevX, iPrevY;

	_ASSERTE(this!=nullptr && this->mp_Grp1==nullptr && this->mp_Grp2==nullptr && this->mp_Item);

	if (!nHorz && !nVert)
	{
		// Just find nearest opposite
		if (!mp_Parent)
			goto wrap;
		// Look at parent split
		if (mp_Parent->m_SplitType == RConStartArgsEx::eSplitVert)
			// Panes are above and below splitter
			nVert = (this == mp_Parent->mp_Grp1) ? 1 : -1;
		else if (mp_Parent->m_SplitType == RConStartArgsEx::eSplitHorz)
			// Panes are leftward and rightward of splitter
			nHorz = (this == mp_Parent->mp_Grp1) ? 1 : -1;
		else
			goto wrap;
	}

	// Get all panes from this grand-group
	nGroupPanes = GetRootGroup()->GetGroupPanes(&Panes);
	if (nGroupPanes < 2)
		goto wrap;

	// Eval the center of our pane
	iPrevX = ((rcPrev.right+rcPrev.left)>>1);
	iPrevY = ((rcPrev.bottom+rcPrev.top)>>1);

	for (int i = 0; i < nGroupPanes; i++)
	{
		CVConGroup* pGrp = (CVConGroup*)(Panes[i]->VCon()->mp_Group);
		if (pGrp == this)
			continue;
		RECT rc = pGrp->mrc_Full;

		// Drop conditions
		if (nHorz && !nVert)
		{
			if (nHorz < 0)
			{
				// Must be on the left of our pane
				if (rc.right > rcPrev.left)
					continue;
			}
			else
			{
				// Must be on the right of our pane
				if (rc.left < rcPrev.right)
					continue;
			}
		}
		else if (nVert && !nHorz)
		{
			if (nVert < 0)
			{
				// Must be above our pane
				if (rc.bottom > rcPrev.top)
					continue;
			}
			else
			{
				// Must be below our pane
				if (rc.top < rcPrev.bottom)
					continue;
			}
		}

		// Drop if direction does not match {nHorz,nVert}
		int iX = ((rc.right+rc.left)>>1);
		int iY = ((rc.bottom+rc.top)>>1);
		if ((nHorz < 0) && (iX > iPrevX))
			continue;
		if ((nHorz > 0) && (iX < iPrevX))
			continue;
		if ((nVert < 0) && (iY > iPrevY))
			continue;
		if ((nVert > 0) && (iY < iPrevY))
			continue;

		// Well, look at "distance"
		int iDX = (iX - iPrevX);
		int iDY = (iY - iPrevY);
		int iDist = iDX*iDX + iDY*iDY;
		// And find the nearest pane
		if (!pNext || (iMinDistance > iDist))
		{
			pNext = pGrp;
			iMinDistance = iDist;
		}
	}

wrap:
	FreePanesArray(Panes);
	if (!pNext)
		pNext = const_cast<CVConGroup*>(this);
	return pNext;
}

bool CVConGroup::ExchangePanes(CVirtualConsole* apVCon, int nHorz /*= 0*/, int nVert /*= 0*/)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	CVConGuard VCon1(apVCon);
	if (!isValid(apVCon))
		return false;
	CVConGroup* pGrp = (CVConGroup*)apVCon->mp_Group;
	if (!pGrp || !pGrp->mp_Parent)
		return false;
	RECT wasRect = pGrp->mrc_Full;

	bool bChanged = false;
	CVConGroup* pNext = pGrp->FindNextPane(wasRect, nHorz, nVert);
	if (pNext)
	{
		if ((pNext != pGrp) && (pNext->mp_Item) && pGrp->mp_Parent && pNext->mp_Parent)
		{
			CVConGuard VCon2(pNext->mp_Item);
			CVConGroup*& g1 = (pGrp->mp_Parent->mp_Grp1 == pGrp) ? pGrp->mp_Parent->mp_Grp1 : pGrp->mp_Parent->mp_Grp2;
			_ASSERTE(g1 == pGrp);
			CVConGroup*& g2 = (pNext->mp_Parent->mp_Grp1 == pNext) ? pNext->mp_Parent->mp_Grp1 : pNext->mp_Parent->mp_Grp2;
			_ASSERTE(g2 == pNext);

			std::swap(g1, g2);
			std::swap(g1->mp_Parent, g2->mp_Parent);

			lockGroups.Unlock();

			TODO("Swap Tabs (gp_VCon) too?");

			apVCon->Owner()->OnSize();
			bChanged = true;
		}
		else
		{
			//_ASSERTE(pNext != pGrp); -- that means, no pane in requested direction...
			_ASSERTE(pNext->mp_Item);
		}
	}

	return bChanged;
}

bool CVConGroup::ActivateNextPane(CVirtualConsole* apVCon, int nHorz /*= 0*/, int nVert /*= 0*/)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	CVConGuard guard(apVCon);
	if (!isValid(apVCon))
		return false;
	CVConGroup* pGrp = (CVConGroup*)apVCon->mp_Group;
	if (!pGrp || !pGrp->mp_Parent)
		return false;
	RECT wasRect = pGrp->mrc_Full;

	bool bActivated = false;
	CVConGroup* pNext = pGrp->FindNextPane(wasRect, nHorz, nVert);
	if (pNext)
	{
		if ((pNext != pGrp) && (pNext->mp_Item))
		{
			bActivated = CVConGroup::Activate(pNext->mp_Item);
		}
		else
		{
			//_ASSERTE(pNext != pGrp); -- that means, no pane in requested direction...
			_ASSERTE(pNext->mp_Item);
		}
	}

	return bActivated;
}

bool CVConGroup::Activate(CVirtualConsole* apVCon)
{
	if (!isValid(apVCon))
		return false;

	bool lbRc = false;

	// Как-то все запутанно, с вызовами ConActivate и разными функциями...

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == apVCon)
		{
			ConActivate(i);
			lbRc = (gp_VActive == apVCon);
			break;
		}
	}

	return lbRc;
}

void CVConGroup::MoveActiveTab(CVirtualConsole* apVCon, bool bLeftward)
{
	if (!apVCon)
		apVCon = gp_VActive;

	bool lbChanged = false;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == apVCon)
		{
			if (bLeftward)
			{
				if (i > 0)
				{
					CVirtualConsole* p = gp_VCon[i-1];
					gp_VCon[i-1] = gp_VCon[i];
					gp_VCon[i] = p;
					apVCon->RCon()->OnActivate(i-1, i);
					lbChanged = true;
				}
			}
			else
			{
				if ((i < (countof(gp_VCon)-1)) && gp_VCon[i+1])
				{
					CVirtualConsole* p = gp_VCon[i+1];
					gp_VCon[i+1] = gp_VCon[i];
					gp_VCon[i] = p;
					apVCon->RCon()->OnActivate(i+1, i);
					lbChanged = true;
				}
			}
			break;
		}
	}

	if (lbChanged)
	{
		gpConEmu->Taskbar_GhostReorder();
	}
}

// 0 - based
int CVConGroup::ActiveConNum()
{
	int nActive = -1;

	if (gp_VActive)
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			if (gp_VCon[i] == gp_VActive)
			{
				nActive = i; break;
			}
		}
	}

	return nActive;
}

int CVConGroup::GetConCount(bool bNoDetached /*= false*/)
{
	int nCount = 0;

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (!gp_VCon[i])
			break;

		if (bNoDetached)
		{
			_ASSERTE(isMainThread()); // чтобы не морочиться с блокировками
			if (gp_VCon[i]->RCon()->isDetached())
				continue;
		}

		nCount++;
	}

	return nCount;
}

BOOL CVConGroup::AttachRequested(HWND ahConWnd, const CESERVER_REQ_STARTSTOP* pStartStop, CESERVER_REQ_SRVSTARTSTOPRET& pRet)
{
	CVConGuard VCon;
	bool bFound = false;
	_ASSERTE(pStartStop->dwPID!=0);

	if (gpSet->isLogging())
	{
		size_t cchAll = 255 + _tcslen(pStartStop->sCmdLine) + _tcslen(pStartStop->sModuleName);
		wchar_t* pszLog = (wchar_t*)malloc(cchAll*sizeof(*pszLog));
		_wsprintf(pszLog, SKIPLEN(cchAll)
			L"Attach requested. HWND=x%08X, AID=%u, PID=%u, Sys=%u, Bit=%u, Adm=%u, Hkl=x%08X, Wnd={%u-%u}, Buf={%u-%u}, Max={%u-%u}, Cmd=%s",
			(DWORD)(DWORD_PTR)pStartStop->hWnd, pStartStop->dwAID, pStartStop->dwPID,
			pStartStop->nSubSystem, pStartStop->nImageBits, pStartStop->bUserIsAdmin, pStartStop->dwKeybLayout,
			pStartStop->sbi.srWindow.Right-pStartStop->sbi.srWindow.Left+1, pStartStop->sbi.srWindow.Bottom-pStartStop->sbi.srWindow.Top+1,
			pStartStop->sbi.dwSize.X, pStartStop->sbi.dwSize.Y,
			pStartStop->crMaxSize.X, pStartStop->crMaxSize.Y,
			pStartStop->sCmdLine[0] ? pStartStop->sCmdLine : pStartStop->sModuleName);
		gpConEmu->LogString(pszLog);
		free(pszLog);
	}

	// Может быть какой-то VCon ждет аттача?
	if (!bFound)
	{
		#ifdef _DEBUG
		if (pStartStop->dwAID == 0)
		{
			//Штатная ситуация, если аттач (запуск ConEmu.exe) инициируется из сервера
			wchar_t szDbg[128];
			//_ASSERTE(pStartStop->dwAID!=0);
			swprintf_c(szDbg, L"Attach was requested from ServerPID=%u without dwAID\n", pStartStop->dwPID);
			DEBUGSTRATTACHERR(szDbg);
		}
		#endif

		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			VCon.Attach(gp_VCon[i]);
			CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
			if (pRCon != nullptr)
			{
				if (pStartStop->dwAID)
				{
					if (pRCon->GetMonitorThreadID() == pStartStop->dwAID)
					{
						bFound = true;
						break;
					}
				}
				else
				{
					if (pRCon->GetServerPID() == pStartStop->dwPID)
					{
						bFound = true;
						break;
					}
				}
			}
		}
	}

	// Если по ИД не нашли - ищем любую "ожидающую" аттача
	if (!bFound)
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			VCon.Attach(gp_VCon[i]);
			CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
			if (pRCon != nullptr)
			{
				if (pRCon->isDetached())
				{
					bFound = true;
					break;
				}
			}
		}
	}

	// Если не нашли - определим, можно ли добавить новую консоль?
	if (!bFound)
	{
		struct createVCon {
			RConStartArgsEx* pArgs;
			const CESERVER_REQ_STARTSTOP* pStartStop;
			CVConGuard VCon;

			static LRESULT createVConProc(LPARAM lParam)
			{
				createVCon* p = (createVCon*)lParam;
				p->VCon.Attach(gpConEmu->CreateCon(*p->pArgs));
				if (!p->VCon.VCon())
					return 0;
				if (p->pStartStop && p->pStartStop->Palette.bPalletteLoaded)
					p->VCon->SetSelfPalette(p->pStartStop->Palette.wAttributes, p->pStartStop->Palette.wPopupAttributes, p->pStartStop->Palette.ColorTable);
				return (LRESULT)p->VCon.VCon();
			};
		} impl = {new RConStartArgsEx(), pStartStop};

		if (!impl.pArgs)
		{
			_ASSERTE(impl.pArgs != nullptr);
			goto wrap;
		}
		impl.pArgs->Detached = crb_On;
		impl.pArgs->BackgroundTab = pStartStop->bRunInBackgroundTab ? crb_On : crb_Undefined;
		impl.pArgs->RunAsAdministrator = pStartStop->bUserIsAdmin ? crb_On : crb_Undefined;
		_ASSERTE(pStartStop->sCmdLine[0]!=0);
		impl.pArgs->pszSpecialCmd = lstrdup(pStartStop->sCmdLine).Detach();

		if (gpConEmu->isIconic())
		{
			gpConEmu->DoMinimizeRestore(sih_SetForeground);
		}

		// Execute this in MainThread
		VCon.Attach((CVirtualConsole*)gpConEmu->CallMainThread(true, createVCon::createVConProc, (LPARAM)&impl));

		if (VCon.VCon() && !isValid(VCon.VCon()))
		{
			_ASSERTE(FALSE && "MsgCreateCon failed");
			VCon.Release();
		}

		bFound = (VCon.VCon() != nullptr);
		//if ((pVCon = CreateCon(&args)) == nullptr)
		//	return FALSE;
	}

	if (!bFound || !VCon.VCon())
	{
		//Assert? Report?
		bFound = FALSE;
	}
	else
	{
		// Пытаемся подцепить консоль
		bFound = VCon->RCon()->AttachConemuC(ahConWnd, pStartStop->dwPID, pStartStop, pRet);
	}

wrap:
	return bFound;
}

CRealConsole* CVConGroup::AttachRequestedGui(DWORD anServerPID, LPCWSTR asAppFileName, DWORD anAppPID)
{
	_ASSERTE(anServerPID!=0);
	CRealConsole* pRCon = nullptr;
	CVConGuard VCon;

	// Logging
	wchar_t szLogInfo[MAX_PATH];
	if (gpSet->isLogging())
	{
		swprintf_c(szLogInfo, L"AttachRequestedGui. SrvPID=%u. AppPID=%u, FileName=", anServerPID, anAppPID);
		lstrcpyn(szLogInfo+_tcslen(szLogInfo), asAppFileName ? asAppFileName : L"<nullptr>", 128);
		LogString(szLogInfo);
	}

	// The loop
	DWORD nStartTick = GetTickCount(), nDelta = 0;
	while (!pRCon && (nDelta <= 2500))
	{
		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			if (VCon.Attach(gp_VCon[i]))
			{
				if (VCon->RCon()->GuiAppAttachAllowed(anServerPID, asAppFileName, anAppPID))
				{
					pRCon = VCon->RCon();
					break;
				}
			}
		}

		if (pRCon)
			break;

		Sleep(100);
		nDelta = (GetTickCount() - nStartTick);
	}

	// Logging
	if (gpSet->isLogging())
	{
		wchar_t szRc[64];
		if (pRCon)
			swprintf_c(szRc, L"Succeeded. ServerPID=%u", pRCon->GetServerPID());
		else
			wcscpy_c(szRc, L"Rejected");
		swprintf_c(szLogInfo, L"AttachRequestedGui. SrvPID=%u. AppPID=%u. %s", anServerPID, anAppPID, szRc);
		LogString(szLogInfo);
	}

	return pRCon;
}

bool CVConGroup::GetVConBySrvPID(DWORD anServerPID, DWORD anMonitorTID, CVConGuard* pVCon)
{
	bool bFound = false;
	CRealConsole* pRCon;
	DWORD nStartTick = GetTickCount(), nDelta = 0;
	const DWORD nDeltaMax = 2500;

	while (!bFound && (nDelta <= nDeltaMax))
	{
		LONG nInCreate = 0;
		CVConGuard VCon;

		for (size_t i = 0; i < countof(gp_VCon); i++)
		{
			if (VCon.Attach(gp_VCon[i]) && (pRCon = VCon->RCon()) != nullptr)
			{
				if ((anMonitorTID && (pRCon->GetMonitorThreadID() == anMonitorTID))
					|| (anServerPID && (pRCon->GetServerPID() == anServerPID)))
				{
					if (pVCon)
						pVCon->Attach(VCon.VCon());
					bFound = true;
					break;
				}
				else if (pRCon->InCreateRoot())
				{
					nInCreate++;
				}
			}
		}

		if (bFound || !nInCreate)
			break;

		Sleep(100);
		nDelta = GetTickCount() - nStartTick;
	}

	Assert(bFound && "Appropriate RCon was not found");

	return bFound;
}

bool CVConGroup::GetVConByHWND(HWND hConWnd, HWND hDcWnd, CVConGuard* pVCon /*= nullptr*/)
{
	struct impl
	{
		HWND hConWnd, hDcWnd;
		CVConGuard* rpVCon;
		bool bFound;
		static bool FindCon(CVirtualConsole* pVCon, LPARAM lParam)
		{
			impl* i = (impl*)lParam;
			if ((i->hConWnd && (pVCon->RCon()->ConWnd() == i->hConWnd))
				|| (i->hDcWnd && (pVCon->GetView() == i->hDcWnd)))
			{
				i->bFound = true;
				if (i->rpVCon)
					i->rpVCon->Attach(pVCon);
				return false;
			}
			return true;
		};
	} Impl = {hConWnd, hDcWnd, pVCon};
	EnumVCon(evf_All, impl::FindCon, (LPARAM)&Impl);
	return Impl.bFound;
}

// asName - renamed title, console title, active process name, root process name
bool CVConGroup::GetVConByName(LPCWSTR asName, CVConGuard* rpVCon /*= nullptr*/)
{
	if (!asName || !*asName)
		return false;

	struct impl
	{
		LPCWSTR pszName;
		CVConGuard VCon;

		enum {
			m_Renamed = 0,
			m_StdTitle,
			m_ActiveExe,
			m_RootExe,
			m_Last
		};
		int  iMode;
		bool bRenamed;

		static bool FindVCon(CVirtualConsole* pVCon, LPARAM lParam)
		{
			impl* p = (impl*)lParam;
			CRealConsole* pRCon = pVCon->RCon();
			if (!pRCon)
				return true; // Try next

			// ‘Renamed’ flags match?
			if (p->bRenamed != ((pRCon->GetActiveTabType() & fwt_Renamed) == fwt_Renamed))
				return true; // Try next

			LPCWSTR pszTitle;
			switch (p->iMode)
			{
			case m_StdTitle:
				pszTitle = pRCon->GetTitle(false);
				break;
			case m_ActiveExe:
				pszTitle = pRCon->GetActiveProcessName();
				break;
			case m_RootExe:
				pszTitle = pRCon->GetRootProcessName();
				break;
			default:
				pszTitle = pRCon->GetTitle(true);
			}

			// Compare insensitively
			if (pszTitle && (lstrcmpi(p->pszName, pszTitle)) == 0)
			{
				p->VCon.Attach(pVCon);
				return false; // Found, stop iterations
			}
			return true;
		};
	} Impl = {asName};

	// Try only renamed tabs first
	Impl.iMode = impl::m_Renamed; Impl.bRenamed = true;
	EnumVCon(evf_All, impl::FindVCon, (LPARAM)&Impl);

	for (int step = 0; !Impl.VCon.VCon() && (step <= 1); step++)
	{
		// To be able activate "cmd.exe" tab which is NOT renamed,
		// we shall find for non-renamed tabs first!
		Impl.bRenamed = (step == 1);
		for (Impl.iMode = impl::m_StdTitle; (Impl.iMode < impl::m_Last) && !Impl.VCon.VCon(); Impl.iMode++)
		{
			EnumVCon(evf_All, impl::FindVCon, (LPARAM)&Impl);
		}
	}

	// Not found
	if (!Impl.VCon.VCon())
		return false;

	// Found
	if (rpVCon)
		rpVCon->Attach(Impl.VCon.VCon());
	return true;
}

// asName - renamed title, console title, active process name, root process name
bool CVConGroup::ConActivateByName(LPCWSTR asName)
{
	CVConGuard VCon;
	if (!GetVConByName(asName, &VCon))
		return false;

	if (VCon->isActive(false/*abAllowGroup*/))
		return true; // Already active

	return Activate(VCon.VCon());
}

// Вернуть общее количество процессов по всем консолям
DWORD CVConGroup::CheckProcesses()
{
	DWORD dwAllCount = 0;

	for (size_t j = 0; j < countof(gp_VCon); j++)
	{
		if (gp_VCon[j])
		{
			int nCount = gp_VCon[j]->RCon()->GetProcesses(nullptr);
			if (nCount)
				dwAllCount += nCount;
		}
	}

	gpConEmu->m_ProcCount = dwAllCount;
	return dwAllCount;
}

bool CVConGroup::ConActivateNext(bool abNext)
{
	int nActive = ActiveConNum(), i, j, n1, n2, n3;

	for (j = 0; j <= 1; j++)
	{
		if (abNext)
		{
			if (j == 0)
			{
				n1 = nActive+1; n2 = countof(gp_VCon); n3 = 1;
			}
			else
			{
				n1 = 0; n2 = nActive; n3 = 1;
			}

			if (n1>=n2) continue;
		}
		else
		{
			if (j == 0)
			{
				n1 = nActive-1; n2 = -1; n3 = -1;
			}
			else
			{
				n1 = countof(gp_VCon)-1; n2 = nActive; n3 = -1;
			}

			if (n1<=n2) continue;
		}

		for (i = n1; i != n2 && i >= 0 && i < (int)countof(gp_VCon); i+=n3)
		{
			if (gp_VCon[i])
			{
				return ConActivate(i);
			}
		}
	}

	return false;
}

// Если rPanes==nullptr - просто вернуть количество сплитов
int CVConGroup::GetGroupPanes(MArray<CVConGuard*>* rPanes)
{
	AssertThisRet(0);

	int nAdd = 0;

	_ASSERTE((m_SplitType==RConStartArgsEx::eSplitNone && mp_Item!=nullptr) || (m_SplitType!=RConStartArgsEx::eSplitNone && (mp_Grp1 || mp_Grp2)));

	if (m_SplitType==RConStartArgsEx::eSplitNone)
	{
		if (mp_Item)
		{
			if (rPanes)
			{
				CVConGuard* pVConG = new CVConGuard(mp_Item);
				rPanes->push_back(pVConG);
			}
			nAdd++;
		}
	}
	else
	{
		if (mp_Grp1)
			nAdd += mp_Grp1->GetGroupPanes(rPanes);
		if (mp_Grp2)
			nAdd += mp_Grp2->GetGroupPanes(rPanes);
	}

	return nAdd;
}

void CVConGroup::FreePanesArray(MArray<CVConGuard*> &rPanes)
{
	for (int i = rPanes.size(); i--;)
	{
		CVConGuard* p = rPanes[i];
		rPanes.erase(i);
		SafeDelete(p);
	}
	_ASSERTE(rPanes.size()==0);
	rPanes.clear();
}

bool CVConGroup::PaneActivateNext(bool abNext)
{
	CVConGuard VCon(gp_VActive);
	if (!gp_VActive)
		return false;

	bool bOk = false;
	CVirtualConsole* pVCon = VCon.VCon();
	CVConGroup* pActiveGroup = GetRootOfVCon(gp_VActive);
	MArray<CVConGuard*> Panes;

	int nCount = pActiveGroup->GetGroupPanes(&Panes);

	if (nCount > 1)
	{
		CVirtualConsole* pVNext = nullptr;

		if (abNext)
		{
			for (int i = 0; i < nCount; i++)
			{
				if (Panes[i]->VCon() == pVCon)
				{
					pVNext = Panes[((i+1) < nCount) ? (i+1) : 0]->VCon();
					break;
				}
			}
		}
		else
		{
			for (int i = nCount; i--;)
			{
				if (Panes[i]->VCon() == pVCon)
				{
					pVNext = Panes[i ? (i-1) : (nCount-1)]->VCon();
					break;
				}
			}
		}


		if (pVNext)
		{
			bOk = Activate(pVNext);
		}
	}

	FreePanesArray(Panes);
	return bOk;
}

void CVConGroup::ShowActiveGroup(CVirtualConsole* pOldActive)
{
	CVConGuard VCon(gp_VActive);
	if (!gp_VActive)
		return;

	CVConGroup* pActiveGroup = GetRootOfVCon(gp_VActive);
	if (pActiveGroup && pActiveGroup->mb_ResizeFlag)
	{
		SyncConsoleToWindow();
		CVConGroup::ReSizePanes(gp_VActive->mp_ConEmu->WorkspaceRect());
	}

	// Showing...
	CVConGroup* pOldGroup = (pOldActive && (pOldActive != gp_VActive)) ? GetRootOfVCon(pOldActive) : nullptr;

	// Теперь можно показать активную
	pActiveGroup->ShowAllVCon(SW_SHOW);
	// и спрятать деактивированную
	if (pOldGroup && (pOldGroup != pActiveGroup))
		pOldGroup->ShowAllVCon(SW_HIDE);

	InvalidateAll();
}

void CVConGroup::OnConActivated(CVirtualConsole* pVCon)
{
	CVConGroup* pRoot = GetRootOfVCon(pVCon);
	if (pRoot)
	{
		pRoot->StoreActiveVCon(pVCon);
	}
}

void CVConGroup::StoreActiveVCon(CVirtualConsole* pVCon)
{
	mp_ActiveGroupVConPtr = pVCon;

	if (m_SplitType!=RConStartArgsEx::eSplitNone)
	{
		if (mp_Grp1)
			mp_Grp1->StoreActiveVCon(pVCon);
		if (mp_Grp2)
			mp_Grp2->StoreActiveVCon(pVCon);
	}
}

bool CVConGroup::ConActivate(CVConGuard& VCon, int nCon)
{
	CVirtualConsole* pVCon = VCon.VCon();

	if (pVCon == nullptr)
	{
		_ASSERTE(pVCon!=nullptr && "VCon was not created");
		return false;
	}

	if (pVCon->RCon() == nullptr)
	{
		_ASSERTE(FALSE && "mp_RCon was not created");
		return false;
	}

	if (pVCon == gp_VActive)
	{
		// Iterate tabs
		int nTabCount;
		CRealConsole *pRCon;

		// By sequental pressing "Win+<Number>" - loop tabs of active console (Far Manager windows)
		if (gpSet->isMultiIterate
			    && ((pRCon = gp_VActive->RCon()) != nullptr)
			    && ((nTabCount = pRCon->GetTabCount())>1))
		{
			int nNextActive = pRCon->GetActiveTab()+1;

			if (nNextActive >= nTabCount)
				nNextActive = 0;

			CTab tab(__FILE__,__LINE__);
			if (pRCon->GetTab(nNextActive, tab))
			{
				int nFarWndId = tab->Info.nFarWindowID;
				if (pRCon->CanActivateFarWindow(nFarWndId))
					pRCon->ActivateFarWindow(nFarWndId);
			}
		}

		return true; // Success
	}

	bool lbSizeOK = true;
	int nOldConNum = ActiveConNum();

	CVirtualConsole* pOldActive = gp_VActive;

	// Hide PictureView and others...
	if (gp_VActive && gp_VActive->RCon())
	{
		gp_VActive->RCon()->OnDeactivate(nCon);
	}

	// BEFORE switching into other console - update its dimensions
	CRealConsole* pRCon = pVCon->RCon();
	int nOldConWidth = pRCon->TextWidth();
	int nOldConHeight = pRCon->TextHeight();
	RECT rcNewCon = gpConEmu->CalcRect(CER_CONSOLE_CUR, pVCon);
	int nNewConWidth = rcNewCon.right;
	int nNewConHeight = rcNewCon.bottom;

	wchar_t szInfo[128];
	swprintf_c(szInfo, L"Activating con #%u, OldSize={%u,%u}, NewSize={%u,%u}",
		nCon, nOldConWidth, nOldConHeight, nNewConWidth, nNewConHeight);
	if (gpSet->isLogging())
	{
		pRCon->LogString(szInfo);
	}
	else
	{
		DEBUGSTRACTIVATE(szInfo);
	}

	if (!pRCon->isServerClosing()
		&& (nOldConWidth != nNewConWidth || nOldConHeight != nNewConHeight))
	{
		DWORD nCmdID = CECMD_SETSIZESYNC;
		if (!pRCon->isFar(true)) nCmdID = CECMD_SETSIZENOSYNC;
		lbSizeOK = pRCon->SetConsoleSize(nNewConWidth, nNewConHeight, 0/*don't change buffer size*/, nCmdID);
	}

	// Correct sizes of VCon/Back
	RECT rcWork = gpConEmu->WorkspaceRect();
	//rcWork = gpConEmu->CalcRect(CER_WORKSPACE, pVCon);
	CVConGroup::MoveAllVCon(pVCon, rcWork);

	setActiveVConAndFlags(pVCon);

	pRCon->OnActivate(nCon, nOldConNum);

	if (!lbSizeOK)
		SyncWindowToConsole(); // -- dummy

	ShowActiveGroup(pOldActive);

	return true; // Success
}

// nCon - zero-based index of console; -1 for last console
bool CVConGroup::ConActivate(int nCon)
{
	FLASHWINFO fl = {sizeof(FLASHWINFO)}; fl.dwFlags = FLASHW_STOP; fl.hwnd = ghWnd;
	FlashWindowEx(&fl); // При многократных созданиях мигать начинает...

	if (nCon == -1)
		nCon = GetConCount() - 1;

	if (nCon >= 0 && nCon < (int)countof(gp_VCon))
	{
		CVConGuard VCon(gp_VCon[nCon]);
		CVirtualConsole* pVCon = VCon.VCon();

		if (pVCon == nullptr)
		{
			if (gpSet->isMultiAutoCreate)
			{
				// Создать новую default-консоль
				gpConEmu->RecreateAction(cra_CreateTab/*FALSE*/, FALSE);
				return true; // создана новая консоль
			}

			return false; // консоль с этим номером не была создана!
		}

		if (pVCon->RCon() == nullptr)
		{
			_ASSERTE(FALSE && "mp_RCon was not created");
			return false;
		}

		_ASSERTE(nCon == VCon->Index()); // Informational, but it must match

		// Do the activation
		return ConActivate(VCon, nCon);
	}

	return false;
}

bool CVConGroup::InCreateGroup()
{
	return gb_InCreateGroup;
}

void CVConGroup::OnCreateGroupBegin()
{
	UINT nFreeCell = 0;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i] == nullptr)
		{
			nFreeCell = i;
			break;
		}
	}
	gn_CreateGroupStartVConIdx = nFreeCell;
	gb_InCreateGroup = true;
}

void CVConGroup::OnCreateGroupEnd()
{
	gn_CreateGroupStartVConIdx = 0;
	gb_InCreateGroup = false;

	// Now it's time to start processes
	gpConEmu->mp_RunQueue->AdvanceQueue();
}

CVirtualConsole* CVConGroup::CreateCon(RConStartArgsEx& args, bool abAllowScripts /*= false*/, bool abForceCurConsole /*= false*/)
{
	if (!isMainThread())
	{
		// Создание VCon в фоновых потоках не допускается, т.к. здесь создаются HWND
		MBoxAssert(isMainThread());
		return nullptr;
	}
	MCHKHEAP;

	CVirtualConsole* pVCon = nullptr;

	// When no command specified - choose default one. Now!
	if ((args.Detached != crb_On) && (!args.pszSpecialCmd || !*args.pszSpecialCmd))
	{
		// We must get here (with empty creation command line) only in certain cases, for example:
		// a) User presses Win+W (or `[+]` button);
		// b) User runs something like "ConEmu -detached GuiMacro Create"
		_ASSERTE((gpConEmu->GetStartupStage() == CConEmuMain::ss_Started || gpConEmu->GetStartupStage() == CConEmuMain::ss_VConStarted) || (gpConEmu->m_StartDetached != crb_Undefined));
		_ASSERTE(args.pszSpecialCmd==nullptr);

		// Сюда мы попадаем, если юзер жмет Win+W (создание без подтверждения)
		LPCWSTR pszSysCmd = gpConEmu->GetCmd(nullptr, !abAllowScripts);
		LPCWSTR pszSysDir = nullptr;
		CVConGuard vActive;
		// Не задано?
		if (!pszSysCmd || !*pszSysCmd)
		{
			// То нельзя запускать _консоль_ с _задачей_ (если !abAllowScripts) или вообще "без команды"
			if (GetActiveVCon(&vActive) >= 0)
			{
				// Попробовать взять команду из текущей консоли?
				pszSysCmd = vActive->RCon()->GetCmd(true);
				if (pszSysCmd && *pszSysCmd && !args.pszStartupDir)
					pszSysDir = vActive->RCon()->GetStartupDir();
				// Run as admin?
				if ((args.RunAsAdministrator == crb_Undefined)
					&& (args.RunAsRestricted == crb_Undefined)
					&& (args.pszUserName == nullptr))
				{
					// That is specially processed because "As admin"
					// may be set in the task with "*" prefix
					if (vActive->RCon()->isAdministrator())
						args.RunAsAdministrator = crb_On;
				}
			}
			// Хм? Команда по умолчанию тогда.
			if (!pszSysCmd || !*pszSysCmd)
			{
				pszSysCmd = gpConEmu->GetDefaultCmd();
			}
		}

		_ASSERTE(args.pszSpecialCmd == nullptr);
		args.pszSpecialCmd = lstrdup(pszSysCmd).Detach();

		if (pszSysDir)
		{
			_ASSERTE(args.pszStartupDir==nullptr);
			args.pszStartupDir = lstrdup(pszSysDir).Detach();
		}

		_ASSERTE(args.pszSpecialCmd && *args.pszSpecialCmd);
	}

	CEStr lsTaskCommands;
	if (args.pszSpecialCmd && *args.pszSpecialCmd)
	{
		// gh-1456: VS Extension runs ConEmu as "ConEmu.exe ... -cmd -cur_console:c {cmd}"
		// Process "-new_console" switches
		args.ProcessNewConArg(abForceCurConsole);

		// E.g.: "{cmd}", "{Shell::PowerShell}"
		if (args.pszSpecialCmd && *args.pszSpecialCmd
			&& gpConEmu->IsConsoleBatchOrTask(args.pszSpecialCmd))
		{
			lsTaskCommands = gpConEmu->LoadConsoleBatch(args.pszSpecialCmd, &args);
		}

		if (lsTaskCommands.IsEmpty() && args.pszSpecialCmd && *args.pszSpecialCmd)
		{
			// Issue 1711: May be that is something like?
			// ""C:\Windows\...\powershell.exe" -noprofile -new_console:t:"PoSh":d:"C:\Users""
			// Start/End quotes need to be removed
			CEStr szExe; NeedCmdOptions opt{};
			const bool bNeedCmd = IsNeedCmd(FALSE, args.pszSpecialCmd, szExe, &opt);
			if (!bNeedCmd && opt.startEndQuot == StartEndQuot::NeedCut)
			{
				const int nLen = lstrlen(args.pszSpecialCmd);
				_ASSERTE(nLen > 4);
				// Cut first quote
				_ASSERTE(args.pszSpecialCmd[0] == L'"' && wcschr(args.pszSpecialCmd + 1, L'"') != nullptr);
				wmemmove(args.pszSpecialCmd, args.pszSpecialCmd + 1, nLen);
				// And trim one end quote
				wchar_t* lastQuot = wcsrchr(args.pszSpecialCmd, L'"');
				if (lastQuot == nullptr)
				{
					_ASSERTE(args.pszSpecialCmd[nLen - 2] == L'"' && args.pszSpecialCmd[nLen - 1] == 0);
				}
				else
				{
					*lastQuot = (*(lastQuot + 1) == L'\0') ? L'\0' : L' ';
				}
			}
			else if (bNeedCmd && opt.startEndQuot == StartEndQuot::NeedAdd)
			{
				CEStr quotedCommand(L"\"", args.pszSpecialCmd, L"\"");
				if (quotedCommand)
				{
					SafeFree(args.pszSpecialCmd);
					args.pszSpecialCmd = quotedCommand.Detach();
				}
			}
		}
	}

	if (gpConEmu->isInside()
		&& gpConEmu->mp_Inside->IsInsideIntegrationAdmin())
	{
		LogString(L"!!! Forcing first console to run as Admin (mb_InsideIntegrationAdmin) !!!");
		gpConEmu->mp_Inside->SetInsideIntegrationAdmin(false);
		args.RunAsAdministrator = crb_On;
	}

	// Support starting new tasks by hotkey in the Active VCon working directory
	// User have to add to Task parameters: /dir "%CD%"
	if (args.pszStartupDir)
	{
		if (lstrcmpi(args.pszStartupDir, L"%CD%") == 0)
		{
			CEStr lsActiveDir;
			CVConGuard vActive;
			if ((GetActiveVCon(&vActive) >= 0) && (vActive->RCon()))
				vActive->RCon()->GetConsoleCurDir(lsActiveDir, true);
			if (lsActiveDir.IsEmpty())
				lsActiveDir.Set(gpConEmu->WorkDir());
			SafeFree(args.pszStartupDir);
			args.pszStartupDir = lsActiveDir.Detach();
		}
	}

	//wchar_t* pszScript = nullptr; //, szScript[MAX_PATH];

	_ASSERTE(args.pszSpecialCmd!=nullptr);

	if ((args.Detached != crb_On)
		&& args.pszSpecialCmd
		&& gpConEmu->IsConsoleBatchOrTask(args.pszSpecialCmd)
		)
	{
		if (!abAllowScripts)
		{
			DisplayLastError(L"Console script are not supported here!", -1);
			return nullptr;
		}

		// As command was specified as {Task} or @TaskFile or group|||of|||commands
		if (lsTaskCommands.IsEmpty())
			return nullptr;

		// GO
		pVCon = gpConEmu->CreateConGroup(lsTaskCommands, &args);

		MCHKHEAP;
		return pVCon;
	}

	// Если на ярлык ConEmu наброшен другой ярлык или программа

	// Ok, Теперь смотрим свободную ячейку в gp_VCon и запускаемся
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (gp_VCon[i])
		{
			CVConGuard VCon(gp_VCon[i]);
			CRealConsole* pRCon = VCon.VCon() ? VCon->RCon() : nullptr;
			if (pRCon && !pRCon->InCreateRoot() && pRCon->isDetached())
			{
				// isDetached() means, that ConEmu.exe was started with "/detached" flag
				// so, it is safe to close "dummy" console, that was created on GUI startup
				pRCon->CloseConsole(false, false);
			}
		}

		if (!gp_VCon[i])
		{
			bool bTabbar = gpConEmu->isTabsShown();
			CVirtualConsole* pOldActive = gp_VActive;
			gb_CreatingActive = true;
			pVCon = CVConGroup::CreateVCon(args, gp_VCon[i], (int)i);
			gb_CreatingActive = false;

			// 130826 - "-new_console:sVb" - "b" was ignored!
			BOOL lbInBackground = (args.BackgroundTab == crb_On) && (pOldActive != nullptr); // && !args.eSplit;
			// 131106 - "cmd -new_console:bsV" fails, split was left invisible
			BOOL lbShowSplit = (args.eSplit != RConStartArgsEx::eSplitNone);

			if (pVCon)
			{
				if (!lbInBackground && pOldActive && pOldActive->RCon())
				{
					pOldActive->RCon()->OnDeactivate(i);
				}

				_ASSERTE(gp_VCon[i] == pVCon);
				gp_VCon[i] = pVCon;

				if (!lbInBackground)
				{
					setActiveVConAndFlags(pVCon);
				}
				else
				{
					_ASSERTE(gp_VActive==pOldActive);
				}

				pVCon->InitGhost();

				if (!lbInBackground)
				{
					pVCon->RCon()->OnActivate(i, ActiveConNum());

					ShowActiveGroup(pOldActive);
				}
				else if (lbShowSplit)
				{
					ShowActiveGroup(nullptr);
				}

			}

			break;
		}
	}

	return pVCon;
}

HRGN CVConGroup::GetExclusionRgn()
{
	HRGN hExclusion = nullptr;
	int iComb = 0;

	// DoubleView: Если видимы несколько консолей - нужно совместить регионы

	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{
			HRGN hVCon = VCon->GetExclusionRgn();

			if (hVCon)
			{
				if (!hExclusion)
				{
					hExclusion = hVCon; // Первый (или единственный)
				}
				else
				{
					iComb = CombineRgn(hExclusion, hExclusion, hVCon, RGN_OR);
					_ASSERTE(iComb != ERROR);
					DeleteObject(hVCon);
				}
			}
		}
	}

	return hExclusion;
}

RECT CVConGroup::CalcRect(enum ConEmuRect tWhat, RECT rFrom, enum ConEmuRect tFrom, CVirtualConsole* pVCon, enum ConEmuMargins tTabAction/*=CEM_TAB*/)
{
	RECT rc = rFrom;
	//RECT rcShift = MakeRect(0,0);

	CVConGuard VCon(pVCon);
	if (!pVCon && (GetActiveVCon(&VCon) >= 0))
	{
		pVCon = VCon.VCon();
	}

	CVConGroup* pGroup = pVCon ? ((CVConGroup*)pVCon->mp_Group) : nullptr;

	// Теперь rc должен соответствовать CER_MAINCLIENT/CER_BACK

#ifdef _DEBUG
	if ((tWhat == CER_DC || tWhat == CER_BACK) && (tFrom == CER_MAIN || tFrom == CER_MAINCLIENT))
	{
		//_ASSERTE(FALSE && "Fails in DoubleView"); // нужно переделать для split, считает по целой области
		bool bDbg = false;
	}
#endif

#if 0
	RECT rcAddShift = MakeRect(0,0);
	if ((tWhat == CER_DC) && (tFrom != CER_CONSOLE_CUR))
	{
		_ASSERTE(pVCon!=nullptr);
		WARNING("warning: DoubleView - тут нужно допиливать");
		RECT rcCalcBack = rFrom;
		// нужно посчитать дополнительные сдвиги
		if (tFrom != CER_BACK)
		{
			_ASSERTE(tFrom==CER_BACK);
			rcCalcBack = gpConEmu->CalcRect(CER_BACK, pVCon);
		}
		//--// расчетный НЕ ДОЛЖЕН быть меньше переданного
		//#ifdef MSGLOGGER
		//_ASSERTE((rcCalcDC.right - rcCalcDC.left)>=(prDC->right - prDC->left));
		//_ASSERTE((rcCalcDC.bottom - rcCalcDC.top)>=(prDC->bottom - prDC->top));
		//#endif
		RECT rcCalcCon = rFrom;
		if (tFrom != CER_CONSOLE_CUR)
		{
			rcCalcCon = CalcRect(CER_CONSOLE_CUR, rcCalcBack, CER_BACK, pVCon);
		}
		// Расчетное DC (размер)
		_ASSERTE(rcCalcCon.left==0 && rcCalcCon.top==0);
		RECT rcCalcDC = MakeRect(0, 0, (rcCalcCon.right * gpFontMgr->FontWidth()), (rcCalcCon.bottom * gpFontMgr->FontHeight()));

		RECT rcScrollPad = gpConEmu->CalcMargins(CEM_SCROLL|CEM_PAD);
		gpConEmu->AddMargins(rcCalcBack, rcScrollPad);

		int nDeltaX = (rcCalcBack.right - rcCalcBack.left) - (rcCalcDC.right - rcCalcDC.left);
		int nDeltaY = (rcCalcBack.bottom - rcCalcBack.top) - (rcCalcDC.bottom - rcCalcDC.top);

		// Теперь сдвиги
		if ((gpSet->isTryToCenter && (gpConEmu->isZoomed() || gpConEmu->isFullScreen() || gpSet->isQuakeStyle))
			|| pVCon->RCon()->isNtvdm())
		{
			// считаем доп.сдвиги. ТОЧНО
			if (nDeltaX > 0)
			{
				rcAddShift.left = nDeltaX >> 1;
				rcAddShift.right = nDeltaX - rcAddShift.left;
			}
		}
		else
		{
			if (nDeltaX > 0)
				rcAddShift.right = nDeltaX;
		}

		if ((gpSet->isTryToCenter && (gpConEmu->isZoomed() || gpConEmu->isFullScreen()))
			|| pVCon->RCon()->isNtvdm())
		{
			if (nDeltaY > 0)
			{
				rcAddShift.top = nDeltaY >> 1;
				rcAddShift.bottom = nDeltaY - rcAddShift.top;
			}
		}
		else
		{
			if (nDeltaY > 0)
				rcAddShift.bottom = nDeltaY;
		}
	}
#endif

	switch (tWhat)
	{
		//case CER_BACK: // switch (tWhat)
		//{
		//	TODO("DoubleView");
		//	RECT rcShift = gpConEmu->CalcMargins(tTabAction|CEM_CLIENT_MARGINS);
		//	CConEmuMain::AddMargins(rc, rcShift);
		//} break;
		case CER_SCROLL: // switch (tWhat)
		{
			RECT rcShift = gpConEmu->CalcMargins(tTabAction);
			CConEmuMain::AddMargins(rc, rcShift);
			rc.left = rc.right - GetSystemMetrics(SM_CXVSCROLL);
			return rc; // Иначе внизу еще будет коррекция по DC (rcAddShift)
		} break;
		case CER_DC: // switch (tWhat)
		case CER_BACK: // switch (tWhat)
		case CER_CONSOLE_ALL: // switch (tWhat)
		case CER_CONSOLE_CUR: // switch (tWhat)
		case CER_CONSOLE_NTVDMOFF: // switch (tWhat)
		{
			_ASSERTE(tWhat!=CER_DC || (tFrom==CER_BACK || tFrom==CER_CONSOLE_CUR || tFrom == CER_MAINCLIENT || tFrom == CER_WORKSPACE));

			COORD crConFixSize = {};
			if ((tWhat == CER_CONSOLE_CUR) && pVCon->RCon() && pVCon->RCon()->isFixAndCenter(&crConFixSize))
			{
				_ASSERTE(crConFixSize.X==80 && (crConFixSize.Y==25 || crConFixSize.Y==28 || crConFixSize.Y==43 || crConFixSize.Y==50));
				rc = MakeRect(crConFixSize.X, crConFixSize.Y);
				return rc;
			}

			if (tFrom == CER_MAINCLIENT)
			{
				// Учесть высоту закладок (табов)
				RECT rcShift = gpConEmu->CalcMargins(tTabAction|CEM_CLIENT_MARGINS);
				CConEmuMain::AddMargins(rc, rcShift);
			}
			else if (tFrom == CER_BACK || tFrom == CER_WORKSPACE)
			{
				// -- отрезание полосы прокрутки ПОСЛЕ разбиения
				//rcShift = gpConEmu->CalcMargins(CEM_SCROLL);
				//CConEmuMain::AddMargins(rc, rcShift);
			}
			else
			{
				// Другие значения - не допускаются
				_ASSERTE(tFrom == CER_MAINCLIENT);
			}

			// Теперь обработка SplitScreen/DoubleView/....
			RECT rcAll = rc;
			if (pGroup && (tWhat != CER_CONSOLE_ALL)
				&& (tFrom == CER_MAINCLIENT || tFrom == CER_WORKSPACE))
			{
				pGroup->CalcSplitRootRect(rcAll, rc);
			}

			// Коррекция отступов
			if ((tFrom == CER_MAINCLIENT || tFrom == CER_BACK || tFrom == CER_WORKSPACE)
				&& (tWhat != CER_BACK))
			{
				// Прокрутка. Пока только справа (планируется и внизу)
				RECT rcShift = gpConEmu->CalcMargins(CEM_SCROLL);
				CConEmuMain::AddMargins(rc, rcShift);
			}

			TODO("Вообще, для CER_CONSOLE_ALL CEM_PAD считать не нужно, но т.к. пока используется SetAllConsoleWindowsSize - оставим");
			if (tWhat == CER_DC || tWhat == CER_CONSOLE_CUR || tWhat == CER_CONSOLE_ALL || tWhat == CER_CONSOLE_NTVDMOFF)
			{
				// Pad. Отступ от всех краев (gpSet->nCenterConsolePad)
				RECT rcShift = gpConEmu->CalcMargins(CEM_PAD);
				CConEmuMain::AddMargins(rc, rcShift);
			}

			//// Для корректного деления на размер знакоместа...
			//         if (gpSetCls->FontWidth()==0 || gpSetCls->FontHeight()==0)
			//             pVCon->InitDC(false, true); // инициализировать ширину шрифта по умолчанию
			//rc.right ++;
			//int nShift = (gpSetCls->FontWidth() - 1) / 2; if (nShift < 1) nShift = 1;
			//rc.right += nShift;
			// Если есть вкладки
			//if (rcShift.top || rcShift.bottom || )
			//nShift = (gpSetCls->FontWidth() - 1) / 2; if (nShift < 1) nShift = 1;

#if 0
			// Если активен NTVDM.
			if ((tWhat != CER_CONSOLE_NTVDMOFF)
				&& (tWhat == CER_DC || tWhat == CER_CONSOLE_CUR)
				&& pVCon && pVCon->RCon() && pVCon->RCon()->isNtvdm())
			{
				// NTVDM устанавливает ВЫСОТУ экранного буфера... в 25/28/43/50 строк
				// путем округления текущей высоты (то есть если до запуска 16bit
				// было 27 строк, то скорее всего будет установлена высота в 28 строк)
				RECT rc1 = MakeRect(pVCon->TextWidth*gpSetCls->FontWidth(), pVCon->TextHeight*gpSetCls->FontHeight());

				//gpSet->ntvdmHeight /* pVCon->TextHeight */ * gpSetCls->FontHeight());
				if (rc1.bottom > (rc.bottom - rc.top))
					rc1.bottom = (rc.bottom - rc.top); // Если размер вылез за текущий - обрежем снизу :(

				int nS = rc.right - rc.left - rc1.right;

				RECT rcShift = {0,0,0,0};

				if (nS>=0)
				{
					rcShift.left = nS / 2;
					rcShift.right = nS - rcShift.left;
				}
				else
				{
					rcShift.left = 0;
					rcShift.right = -nS;
				}

				nS = rc.bottom - rc.top - rc1.bottom;

				if (nS>=0)
				{
					rcShift.top = nS / 2;
					rcShift.bottom = nS - rcShift.top;
				}
				else
				{
					rcShift.top = 0;
					rcShift.bottom = -nS;
				}

				CConEmuMain::AddMargins(rc, rcShift);
			}
#endif

			// Учесть сплиты
			if ((tWhat == CER_CONSOLE_ALL) && gp_VActive && isVConExists(0))
			{
				SIZE Splits = {};
				RECT rcMin = CVConGroup::AllTextRect(&Splits, true);
				if ((Splits.cx > 0) && (gpSet->nSplitWidth > 0))
					rc.right -= Splits.cx * gpSetCls->EvalSize(gpSet->nSplitWidth, esf_Horizontal|esf_CanUseDpi);
				if ((Splits.cy > 0) && (gpSet->nSplitHeight > 0))
					rc.bottom -= Splits.cy * gpSetCls->EvalSize(gpSet->nSplitHeight, esf_Vertical|esf_CanUseDpi);
			}

			// Если нужен размер консоли в символах сразу делим и выходим
			if (tWhat == CER_CONSOLE_ALL || tWhat == CER_CONSOLE_CUR || tWhat == CER_CONSOLE_NTVDMOFF)
			{
				//2009-07-09 - ClientToConsole использовать нельзя, т.к. после его
				//  приближений высота может получиться больше Ideal, а ширина - меньше
				//120822 - было "(rc.right - rc.left + 1)"
				int nW = (rc.right - rc.left) / gpFontMgr->FontWidth();
				int nH = (rc.bottom - rc.top) / gpFontMgr->FontHeight();
				rc.left = 0; rc.top = 0; rc.right = nW; rc.bottom = nH;

				//2010-01-19 - Sort of Windows95 'Auto' option in the font size list box
				if (gpSet->isFontAutoSize)
				{
					// При масштабировании шрифта под размер окна (в пикселях)
					// требуется зафиксировать размер консоли (в символах)
					// Эта ветка гарантирует, что размер не превысит заданный в настройках

					SIZE curSize = gpConEmu->GetDefaultSize(true);

					if (curSize.cx && rc.right > curSize.cx)
						rc.right = curSize.cx;

					if (curSize.cy && rc.bottom > curSize.cy)
						rc.bottom = curSize.cy;
				}

				#ifdef _DEBUG
				_ASSERTE(rc.bottom>=MIN_CON_HEIGHT);
				#endif

				// Возможно, что в RealConsole выставлен большой шрифт, который помешает установке этого размера
				if (pVCon)
				{
					CRealConsole* pRCon = pVCon->RCon();

					if (pRCon)
					{
						COORD crMaxConSize = {0,0};

						// Даже если открыто несколько панелей - не разрешим устанавливать
						// в общей сложности больший размер, т.к. при закрытии одной панели
						// консоль слетит (превысит максимально допустимый размер)
						if (pRCon->GetMaxConSize(&crMaxConSize))
						{
							if (rc.right > crMaxConSize.X)
								rc.right = crMaxConSize.X;

							if (rc.bottom > crMaxConSize.Y)
								rc.bottom = crMaxConSize.Y;
						}
					}
				}
			}
			else
			{
				// Precise DC size, center if required
				RECT rcAddShift = {};
				_ASSERTE(pVCon!=nullptr);
				if (pVCon && (tWhat == CER_DC) && (tFrom != CER_CONSOLE_CUR))
				{
					RECT rcCalcBack = rFrom;
					if (tFrom != CER_BACK)
					{
						if (tFrom == CER_MAINCLIENT)
						{
							rcCalcBack = CVConGroup::CalcRect(CER_BACK, rFrom, tFrom, pVCon, tTabAction);
						}
						else
						{
							_ASSERTE(tFrom == CER_BACK);
							rcCalcBack = CVConGroup::CalcRect(CER_BACK, gpConEmu->ClientRect(), CER_MAINCLIENT, pVCon, tTabAction);
						}
					}
					rcAddShift = pVCon->CalcDCMargins(rcCalcBack);
				}
				CConEmuMain::AddMargins(rc, rcAddShift, rcop_Shrink);
			}
		}
		break;

	default:
		_ASSERTE(FALSE && "No supported value");
		break;
	}

	return rc;
}

void CVConGroup::SetConsoleSizes(const COORD& size, const RECT& rcNewCon, bool abSync)
{
	if (!this)
		return;
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	CGroupGuard Grp(mp_Grp1);
	CVConGuard VCon(mp_Item);

	// Некорректно. Нужно прокрутку просто вводить. А игнорировать установку размера окна нельзя.
#if 0
	// Это не совсем корректно... ntvdm.exe не выгружается после выхода из 16бит приложения
	if (isNtvdm())
	{
		//if (size.X == 80 && size.Y>25 && lastSize1.X != size.X && size.Y == lastSize1.Y) {
		TODO("Ntvdm почему-то не всегда устанавливает ВЫСОТУ консоли в 25/28/50 символов...")
		//}
		return; // запрет изменения размеров консоли для 16бит приложений
	}
#endif

	//#ifdef MSGLOGGER
	//	char szDbg[100]; wsprintfA(szDbg, "SetAllConsoleWindowsSize({%i,%i},%i)\n", size.X, size.Y, updateInfo);
	//	DEBUGLOGFILE(szDbg);
	//#endif
	//	//g_LastConSize = size;

	if (isPictureView())
	{
		lockGroups.Unlock();
		_ASSERTE(FALSE && "isPictureView() must distinct by panes/consoles");
		gpConEmu->isPiewUpdate = true;
		return;
	}




	// Заблокируем заранее
	CGroupGuard Grp1(mp_Grp1);
	CGroupGuard Grp2(mp_Grp2);
	CVConGuard VCon1(mp_Grp1 ? mp_Grp1->mp_Item : nullptr);
	CVConGuard VCon2(mp_Grp2 ? mp_Grp2->mp_Item : nullptr);

	lockGroups.Unlock();

	if ((m_SplitType == RConStartArgsEx::eSplitNone) || !mp_Grp1 || !mp_Grp2)
	{
		_ASSERTE(mp_Grp1==nullptr && mp_Grp2==nullptr);


		RECT rcCon = MakeRect(size.X,size.Y);

		if (VCon.VCon() && VCon->RCon()
			&& !VCon->RCon()->isServerClosing()
			&& ((!(VCon->mn_Flags & vf_Maximized)) || (VCon->mn_Flags & vf_Active))
			&& !VCon->RCon()->isFixAndCenter()
			)
		{
			CRealConsole* pRCon = VCon->RCon();
			COORD CurSize = {(SHORT)pRCon->TextWidth(), (SHORT)pRCon->TextHeight()};

			// split-size-calculation by COORD only - fails. Need to use pixels...

			//RECT calcSize = gpConEmu->CalcRect(CER_CONSOLE_CUR, VCon.VCon());
			//_ASSERTE(calcSize.left==0 && calcSize.top==0 && calcSize.right>0 && calcSize.bottom>0);

			if ((CurSize.X != size.X) || (CurSize.Y != size.Y))
			{
				if (!VCon->RCon()->SetConsoleSize(size.X, size.Y,
					0/*don't change scroll*/, abSync ? CECMD_SETSIZESYNC : CECMD_SETSIZENOSYNC))
				{
					// Debug purposes
					rcCon = MakeRect(VCon->GetTextWidth(), VCon->GetTextHeight());
				}
			}
		}

		return;
	}


	// Do Split

	RECT rcCon1, rcCon2, rcSplitter, rcSize1, rcSize2;
	CalcSplitRect(mn_SplitPercent10, rcNewCon, rcCon1, rcCon2, rcSplitter);

	rcSize1 = CalcRect(CER_CONSOLE_CUR, rcCon1, CER_BACK, VCon1.VCon());
	rcSize2 = CalcRect(CER_CONSOLE_CUR, rcCon2, CER_BACK, VCon2.VCon());

	// Console support only SHORT as coordinates
	COORD sz1 = MakeCoord(rcSize1.right, rcSize1.bottom);
	COORD sz2 = MakeCoord(rcSize2.right, rcSize2.bottom);

	mp_Grp1->SetConsoleSizes(sz1, rcCon1, abSync);
	mp_Grp2->SetConsoleSizes(sz2, rcCon2, abSync);
}

// size in columns and lines
// здесь нужно привести размер GUI к "общей" ширине консолей в символах
// т.е. если по горизонтали есть 2 консоли, и просят размер 80x25
// то эти две консоли должны стать 40x25 а GUI отресайзиться под 80x25
// В принципе, эту функцию можно было бы и в CConEmu оставить, но для общности путь здесь будет
void CVConGroup::SetAllConsoleWindowsSize(RECT rcWorkspace, COORD size, bool bSetRedraw /*= false*/)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	CVConGuard VCon(gp_VActive);
	CGroupGuard Root(GetRootOfVCon(VCon.VCon()));
	lockGroups.Unlock();

	// Некорректно. Нужно прокрутку просто вводить. А игнорировать установку размера окна нельзя.
	#if 0
	// Это не совсем корректно... ntvdm.exe не выгружается после выхода из 16бит приложения
	if (isNtvdm())
	{
		//if (size.X == 80 && size.Y>25 && lastSize1.X != size.X && size.Y == lastSize1.Y) {
		TODO("Ntvdm почему-то не всегда устанавливает ВЫСОТУ консоли в 25/28/50 символов...")
		//}
		return; // запрет изменения размеров консоли для 16бит приложений
	}
	#endif

	#ifdef MSGLOGGER
	char szDbg[100]; wsprintfA(szDbg, "SetAllConsoleWindowsSize({%i,%i},%i)\n", size.X, size.Y, bSetRedraw);
	DEBUGLOGFILE(szDbg);
	#endif

	g_LastConSize = size;

	if (!Root.VGroup())
	{
		_ASSERTE(Root.VGroup() && "Must be defined already!");
		return;
	}

	if (isPictureView())
	{
		gpConEmu->isPiewUpdate = true;
		return;
	}

	RECT rcCon = MakeRect(size.X,size.Y);

	if (bSetRedraw /*&& gp_VActive*/)
	{
		SetRedraw(FALSE);
	}

	// Для разбиения имеет смысл использовать текущий размер окна в пикселях
	//RECT rcWorkspace = gpConEmu->CalcRect(CER_WORKSPACE, rcWnd, tFrom);

	// Избежать мерцания панелей в Far
	if (!bSetRedraw && Root->mp_Item && Root->m_SplitType == RConStartArgsEx::eSplitNone)
	{
		bSetRedraw = Root->mp_Item->RCon()->isFar(true);
	}

	// Go (size real consoles)
	Root->SetConsoleSizes(size, rcWorkspace, bSetRedraw/*as Sync*/);

	if (bSetRedraw /*&& gp_VActive*/)
	{
		SetRedraw(TRUE);
		Redraw();
	}

	MCHKHEAP;
}

void CVConGroup::SyncAllConsoles2Window(RECT rcWorkspace, bool bSetRedraw /*= false*/)
{
	if (!isVConExists(0))
		return;
	CVConGuard VCon(gp_VActive);
	RECT rcAllCon = gpConEmu->CalcRect(CER_CONSOLE_ALL, rcWorkspace, CER_WORKSPACE, VCon.VCon());
	COORD crNewAllSize = {(SHORT)rcAllCon.right,(SHORT)rcAllCon.bottom};
	SetAllConsoleWindowsSize(rcWorkspace, crNewAllSize, bSetRedraw);
}

void CVConGroup::LockSyncConsoleToWindow(bool abLockSync)
{
	gb_SkipSyncSize = abLockSync;
}

// Изменить размер консоли по размеру окна (главного)
void CVConGroup::SyncConsoleToWindow(LPRECT prcNewWnd/*=nullptr*/, bool bSync/*=false*/)
{
	if (gb_SkipSyncSize || gpConEmu->IsWindowModeChanging() || isNtvdm())
		return;

	CVConGuard VCon(gp_VActive);
	if (!VCon.VCon())
		return;

	_ASSERTE(gpConEmu->mn_InResize <= 1);

	#ifdef _DEBUG
	// Не должно вызываться в процессе изменения режима окна
	if (gpConEmu->changeFromWindowMode != wmNotChanging)
	{
		_ASSERTE(gpConEmu->changeFromWindowMode == wmNotChanging);
	}
	#endif

	if (prcNewWnd)
		VCon->mp_ConEmu->RequestSize(RectWidth(*prcNewWnd), RectHeight(*prcNewWnd));

	SyncAllConsoles2Window(VCon->mp_ConEmu->WorkspaceRect(), bSync/*abSetRedraw*/);
}

// -- функция пустая, игнорируется
// Установить размер основного окна по текущему размеру gp_VActive
void CVConGroup::SyncWindowToConsole()
{
	TODO("warning: Допилить прокрутки, чтобы при изменении размера в КОНСОЛИ ее можно было показать в ConEmu");

#if 0
	_ASSERTE(FALSE && "May be this function must be eliminated!");
	// Наверное имеет смысл вообще не ресайзить GUI по размеру консоли.
	// Если консоль больше - прокрутка, если меньше - центрирование.

	DEBUGLOGFILE("SyncWindowToConsole\n");

	if (gb_SkipSyncSize || !gp_VActive)
		return;

#ifdef _DEBUG
	_ASSERTE(GetCurrentThreadId() == gpConEmu->mn_MainThreadId);
	if (gp_VActive->TextWidth == 80)
	{
		int nDbg = gp_VActive->TextWidth;
	}
#endif

	CVConGuard VCon(gp_VActive);
	CRealConsole* pRCon = gp_VActive->RCon();

	if (pRCon && (gp_VActive->TextWidth != pRCon->TextWidth() || gp_VActive->TextHeight != pRCon->TextHeight()))
	{
		_ASSERTE(FALSE);
		gp_VActive->Update();
	}

	RECT rcDC = gp_VActive->GetRect();
	/*MakeRect(gp_VActive->Width, gp_VActive->Height);
	if (gp_VActive->Width == 0 || gp_VActive->Height == 0) {
	rcDC = MakeRect(gp_VActive->winSize.X, gp_VActive->winSize.Y);
	}*/
	//_ASSERTE(rcDC.right>250 && rcDC.bottom>200);
	RECT rcWnd = CalcRect(CER_MAIN, rcDC, CER_DC, gp_VActive); // размеры окна
	//GetCWShift(ghWnd, &cwShift);
	RECT wndR; GetWindowRect(ghWnd, &wndR); // текущий XY

	if (gpSet->isLogging())
	{
		char szInfo[128]; wsprintfA(szInfo, "SyncWindowToConsole(Cols=%i, Rows=%i)", gp_VActive->TextWidth, gp_VActive->TextHeight);
		CVConGroup::LogString(szInfo);
	}

	gpSetCls->UpdateSize(gp_VActive->TextWidth, gp_VActive->TextHeight);
	MOVEWINDOW(ghWnd, wndR.left, wndR.top, rcWnd.right, rcWnd.bottom, 1);
#endif
}

//// Это некие сводные размеры, соответствующие тому, как если бы была
//// только одна активная консоль, БЕЗ Split-screen
//uint CVConGroup::TextWidth()
//{
//	uint nWidth = gpSet->_wndWidth;
//	if (!gp_VActive)
//	{
//		_ASSERTE(FALSE && "No active VCon");
//	}
//	else
//	{
//		CVConGuard VCon(gp_VActive);
//		CVConGroup* p = nullptr;
//		if (gp_VActive && gp_VActive->mp_Group)
//		{
//			p = ((CVConGroup*)gp_VActive->mp_Group)->GetRootGroup();
//		}
//
//		if (p != nullptr)
//		{
//			SIZE sz; p->GetAllTextSize(sz);
//			nWidth = sz.cx; // p->AllTextWidth();
//		}
//		else
//		{
//			_ASSERTE(p && "CVConGroup MUST BE DEFINED!");
//
//			if (gp_VActive->RCon())
//			{
//				// При ресайзе через окно настройки - gp_VActive еще не перерисовался
//				// так что и TextWidth/TextHeight не обновился
//				//-- gpSetCls->UpdateSize(gp_VActive->TextWidth, gp_VActive->TextHeight);
//				nWidth = gp_VActive->RCon()->TextWidth();
//			}
//		}
//	}
//	return nWidth;
//}
//uint CVConGroup::TextHeight()
//{
//	uint nHeight = gpSet->_wndHeight;
//	if (!gp_VActive)
//	{
//		_ASSERTE(FALSE && "No active VCon");
//	}
//	else
//	{
//		CVConGuard VCon(gp_VActive);
//		CVConGroup* p = nullptr;
//		if (gp_VActive && gp_VActive->mp_Group)
//		{
//			p = ((CVConGroup*)gp_VActive->mp_Group)->GetRootGroup();
//		}
//
//		if (p != nullptr)
//		{
//			SIZE sz; p->GetAllTextSize(sz);
//			nHeight = sz.cy; // p->AllTextHeight();
//		}
//		else
//		{
//			_ASSERTE(p && "CVConGroup MUST BE DEFINED!");
//
//			if (gp_VActive->RCon())
//			{
//				// При ресайзе через окно настройки - gp_VActive еще не перерисовался
//				// так что и TextWidth/TextHeight не обновился
//				//-- gpSetCls->UpdateSize(gp_VActive->TextWidth, gp_VActive->TextHeight);
//				nHeight = gp_VActive->RCon()->TextHeight();
//			}
//		}
//	}
//	return nHeight;
//}

RECT CVConGroup::AllTextRect(SIZE* rpSplits /*= nullptr*/, bool abMinimal /*= false*/)
{
	RECT rcText = MakeRect(MIN_CON_WIDTH,MIN_CON_HEIGHT);
	SIZE Splits = {};

	if (!gp_VActive)
	{
		_ASSERTE(FALSE && "No active VCon");
	}
	else
	{
		CVConGuard VCon(gp_VActive);
		CVConGroup* p = nullptr;
		if (gp_VActive && gp_VActive->mp_Group)
		{
			p = ((CVConGroup*)gp_VActive->mp_Group)->GetRootGroup();
		}

		if (p != nullptr)
		{
			SIZE sz = {MIN_CON_WIDTH,MIN_CON_HEIGHT};
			p->GetAllTextSize(sz, Splits, abMinimal);
			rcText.right = sz.cx;
			rcText.bottom = sz.cy;
		}
		else
		{
			_ASSERTE(p && "CVConGroup MUST BE DEFINED!");

			if (!abMinimal && gp_VActive->RCon())
			{
				// При ресайзе через окно настройки - gp_VActive еще не перерисовался
				// так что и TextWidth/TextHeight не обновился
				//-- gpSetCls->UpdateSize(gp_VActive->TextWidth, gp_VActive->TextHeight);
				int nWidth = gp_VActive->RCon()->TextWidth();
				int nHeight = gp_VActive->RCon()->TextHeight();
				if (nWidth <= 0 || nHeight <= 0)
				{
					_ASSERTE(FALSE && "gp_VActive->RCon()->TextWidth()>=0 && gp_VActive->RCon()->TextHeight()>=0");
				}
				else
				{
					rcText.right = nWidth;
					rcText.bottom = nHeight;
				}
			}
		}
	}

	if (rpSplits)
		*rpSplits = Splits;

	return rcText;
}

// WindowMode: rNormal, rMaximized, rFullScreen
// rcWnd: размер ghWnd
// Returns: true - если успешно, можно продолжать
// #SIZE_TODO Replace rcWnd with SIZE?
bool CVConGroup::PreReSize(unsigned WindowMode, RECT rcWnd, enum ConEmuRect tFrom /*= CER_MAIN*/, bool bSetRedraw /*= false*/)
{
	const bool lbRc = true;
	CVConGuard VCon(gp_VActive);

	_ASSERTEX(tFrom == CER_MAIN || tFrom == CER_MAINCLIENT);

	if (gp_VActive)
	{
		gpConEmu->AutoSizeFont(rcWnd, tFrom);
	}

	if (tFrom == CER_MAIN)
	{
		gpConEmu->RequestSize(RectWidth(rcWnd), RectHeight(rcWnd));
		rcWnd = gpConEmu->ClientRect();
		tFrom = CER_MAINCLIENT;
	}

	RECT rcCon = CalcRect(CER_CONSOLE_ALL, rcWnd, tFrom, gp_VActive);

	if (!rcCon.right || !rcCon.bottom)
	{
		// #Inside: Could happens during initialization of inside mode
		_ASSERTE(rcCon.right && rcCon.bottom);
		rcCon.right = DEF_CON_WIDTH;
		rcCon.bottom = DEF_CON_HEIGHT;
	}

	const COORD size = MakeCoord(rcCon.right, rcCon.bottom);
	if (isVConExists(0))
	{
		SetAllConsoleWindowsSize(rcWnd, size, bSetRedraw);
	}

	//if (bSetRedraw /*&& gp_VActive*/)
	//{
	//	SetRedraw(FALSE);
	//}

	//TODO("DoubleView");
	//if (gp_VActive && !gp_VActive->RCon()->SetConsoleSize(rcCon.right, rcCon.bottom))
	//{
	//	LogString("!!!SetConsoleSize FAILED!!!");

	//	lbRc = false;
	//}

	//if (bSetRedraw /*&& gp_VActive*/)
	//{
	//	SetRedraw(TRUE);
	//	Redraw();
	//}

	return lbRc;
}

void CVConGroup::SetRedraw(bool abRedrawEnabled)
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon(gp_VCon[i]);
		if (VCon.VCon() && VCon->isVisible())
			VCon->SetRedraw(abRedrawEnabled);
	}
}

void CVConGroup::Redraw()
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
			VCon->Redraw();
	}
}

void CVConGroup::InvalidateGaps()
{
	if (ghWndWork)
	{
		InvalidateRect(ghWndWork, nullptr, FALSE);
	}
	else
	{
		_ASSERTE(ghWndWork!=nullptr);
	}

#if 0
	int iRc = SIMPLEREGION;

	RECT rc = {};
	GetClientRect(ghWnd, &rc);
	HRGN h = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);

	#if 0
	TODO("DoubleView");
	if ((iRc != NULLREGION) && mp_TabBar->GetRebarClientRect(&rc))
	{
		HRGN h2 = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
		iRc = CombineRgn(h, h, h2, RGN_DIFF);
		DeleteObject(h2);

		if (iRc == NULLREGION)
			goto wrap;
	}

	if ((iRc != NULLREGION) && mp_Status->GetStatusBarClientRect(&rc))
	{
		HRGN h2 = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
		CombineRgn(h, h, h2, RGN_DIFF);
		DeleteObject(h2);

		if (iRc == NULLREGION)
			goto wrap;
	}
	#endif

	// Теперь - VConsole (все видимые!)
	if (iRc != NULLREGION)
	{
		TODO("DoubleView");
		TODO("Заменить на Background, когда будет");
		HWND hView = gp_VActive ? gp_VActive->GetView() : nullptr;
		if (hView && GetWindowRect(hView, &rc))
		{
			MapWindowPoints(nullptr, ghWnd, (LPPOINT)&rc, 2);
			HRGN h2 = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
			iRc = CombineRgn(h, h, h2, RGN_DIFF);
			DeleteObject(h2);

			if (iRc == NULLREGION)
				goto wrap;
		}
	}

	InvalidateRgn(ghWnd, h, FALSE);

wrap:
	DeleteObject(h);
#endif
}

void CVConGroup::OnPaintSplitter(HDC hdc, HBRUSH hbr)
{
	if (mp_Grp1 && mp_Grp2)
	{
		CVConGuard VCon1(mp_Grp1 ? mp_Grp1->mp_Item : nullptr);
		CVConGuard VCon2(mp_Grp2 ? mp_Grp2->mp_Item : nullptr);

		TODO("DoubleView: красивая отрисовка выпуклых сплиттеров");
		if (IsRectEmpty(&mrc_Splitter))
		{
			_ASSERTE(FALSE && "mrc_Splitter is empty");
		}
		else
		{
			RECT rcPaint = mrc_Splitter;
			MapWindowPoints(ghWnd, ghWndWork, (LPPOINT)&rcPaint, 2);
			FillRect(hdc, &rcPaint, hbr);
		}

		if (mp_Grp1)
			mp_Grp1->OnPaintSplitter(hdc, hbr);
		if (mp_Grp2)
			mp_Grp2->OnPaintSplitter(hdc, hbr);
	}
}

// Должно вызываться ТОЛЬКО для DC в ghWndWork!!!
void CVConGroup::OnPaintGaps(HDC hDC)
{
	bool lbReleaseDC = false;

	if (hDC == nullptr)
	{
		hDC = GetDC(ghWnd); // Главное окно!
		lbReleaseDC = true;
	}

	HBRUSH hBrush = nullptr;


	bool lbFade = gpSet->isFadeInactive && !gpConEmu->isMeForeground(true);


	_ASSERTE(ghWndWork!=nullptr);
	RECT rcClient = {};
	GetClientRect(ghWndWork, &rcClient);

	CVConGuard VCon;
	if (GetActiveVCon(&VCon) < 0)
	{
		const ColorPalette* pDefPal = gpSet->PaletteGet(-1);
		BYTE nColorIdx = 0/*Black*/; // RELEASEDEBUGTEST(0/*Black*/,1/*Blue*/);
		if (pDefPal && pDefPal->nBackColorIdx <= 15)
			nColorIdx = pDefPal->nBackColorIdx;
		HBRUSH hBrush = CreateSolidBrush(gpSet->GetColors(-1, lbFade)[nColorIdx]);

		FillRect(hDC, &rcClient, hBrush);

		SafeDeleteObject(hBrush);
	}
	else
	{
		COLORREF crBack = lbFade ? gpSet->GetFadeColor(gpSet->nStatusBarBack) : gpSet->nStatusBarBack;
		#if 0
		COLORREF crText = lbFade ? gpSet->GetFadeColor(gpSet->nStatusBarLight) : gpSet->nStatusBarLight;
		COLORREF crDash = lbFade ? gpSet->GetFadeColor(gpSet->nStatusBarDark) : gpSet->nStatusBarDark;
		#endif

		hBrush = CreateSolidBrush(crBack);

		MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
		CVConGroup* pRoot = GetRootOfVCon(VCon.VCon());
		if (pRoot)
		{
			pRoot->OnPaintSplitter(hDC, hBrush);
		}
	}

	if (hBrush)
		DeleteObject(hBrush);

	if (lbReleaseDC)
		ReleaseDC(ghWnd, hDC);
}

DWORD CVConGroup::GetFarPID(bool abPluginRequired /*= false*/)
{
	DWORD dwPID = 0;

	if (gp_VActive && gp_VActive->RCon())
		dwPID = gp_VActive->RCon()->GetFarPID(abPluginRequired);

	return dwPID;
}

// Чтобы при создании ПЕРВОЙ консоли на экране сразу можно было что-то нарисовать
void CVConGroup::OnVConCreated(CVirtualConsole* apVCon, const RConStartArgsEx *args)
{
	if (!gp_VActive || (gb_CreatingActive && (args->BackgroundTab != crb_On)))
	{
		setActiveVConAndFlags(apVCon);

		HWND hWndDC = gp_VActive->GetView();
		if (hWndDC != nullptr)
		{
			_ASSERTE(hWndDC==nullptr && "Called from constructor, nullptr expected");
			// Теперь можно показать созданную консоль
			apiShowWindow(gp_VActive->GetView(), SW_SHOW);
		}
	}
}

void CVConGroup::setActiveVConAndFlags(CVirtualConsole* apNewVConActive)
{
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	//TODO: lockVCons

	if (apNewVConActive && !isValid(apNewVConActive))
	{
		_ASSERTE(FALSE && "apNewVConActive has invalid value!");
		apNewVConActive = nullptr;
	}

	gp_VActive = apNewVConActive;
	CVConGroup* pActiveGrp = apNewVConActive ? GetRootOfVCon(apNewVConActive) : nullptr;

	// !!!   Do NOT use EnumVCon here because   !!!
	// !!! EnumVCon uses flags must be set here !!!

	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			DEBUGTEST(VConFlags oldFlags = VCon->mn_Flags);
			VConFlags newFlags = vf_None;

			if (apNewVConActive && (VCon.VCon() == apNewVConActive))
				newFlags |= vf_Active;

			if (pActiveGrp && (GetRootOfVCon(VCon.VCon()) == pActiveGrp))
				newFlags |= vf_Visible;

			VCon->SetFlags(newFlags, vf_Active|vf_Visible, (int)i);
		}
	}
}

void CVConGroup::ResetGroupInput(CConEmuMain* pOwner, GroupInputCmd cmd)
{
	bool set_group;
	if (cmd == gic_Switch)
		set_group = !pOwner->mb_GroupInputFlag;
	else
		set_group = (cmd == gic_Enable);
	// global flag for gpConEmu
	pOwner->mb_GroupInputFlag = set_group;

	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);

	CVConGuard VCon;
	for (size_t i = 0; i < countof(gp_VGroups); i++)
	{
		if (gp_VGroups[i])
		{
			gp_VGroups[i]->mb_GroupInputFlag = false;

			if (gp_VGroups[i]->mp_Item && VCon.Attach(gp_VGroups[i]->mp_Item))
			{
				VCon->SetFlags(vf_None, vf_GroupSplit|vf_GroupSet);
			}
		}
	}

	lockGroups.Unlock();
}

void CVConGroup::GroupInput(CVirtualConsole* apVCon, GroupInputCmd cmd)
{
	CVConGuard VCon;
	bool bGrouped = false;

	if (!VCon.Attach(apVCon))
		return;

	// We'll set group input for active splits
	CVConGroup* pGr = ((CVConGroup*)VCon->mp_Group);
	_ASSERTE(pGr);
	if (pGr && (cmd == gic_Switch))
		bGrouped = !pGr->mb_GroupInputFlag;
	else
		bGrouped = (cmd == gic_Enable);

	// Reset "all consoles are grouped" flag
	if (VCon->Owner()->mb_GroupInputFlag)
		ResetGroupInput(VCon->Owner(), gic_Disable);

	// And for active group...
	CVConGroup* pActiveGrp = GetRootOfVCon(VCon.VCon());

	VConFlags Set = bGrouped ? vf_GroupSplit : vf_None;

	// !!!   Do NOT use EnumVCon here because   !!!
	// !!! EnumVCon uses flags must be set here !!!

	// Update flags
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			if (GetRootOfVCon(VCon.VCon()) != pActiveGrp)
				continue;

			pGr = ((CVConGroup*)apVCon->mp_Group);
			_ASSERTE(pGr);
			while (pGr)
			{
				pGr->mb_GroupInputFlag = bGrouped;
				pGr = pGr->mp_Parent;
			}

			VCon->SetFlags(Set, vf_GroupSplit|vf_GroupSet, (int)i);
		}
	}
}

void CVConGroup::GroupSelectedInput(CVirtualConsole* apVCon)
{
	CVConGuard VCon;
	if (!VCon.Attach(apVCon))
		return;

	if (VCon->Owner()->mb_GroupInputFlag)
	{
		ResetGroupInput(VCon->Owner(), gic_Disable);
	}
	else if (VCon->isGroupedInput() == evf_Visible)
	{
		GroupInput(apVCon, gic_Disable);
	}

	VCon->SetFlags((VCon->isGroupedInput() != evf_GroupSet) ? vf_GroupSet : vf_None, vf_GroupSplit|vf_GroupSet);
}

void CVConGroup::PaneMaximizeRestore(CVirtualConsole* apVCon)
{
	CVConGuard VConActive;
	if (!VConActive.Attach(apVCon))
		return;

	bool bMaximized = false;
	DEBUGTEST(VConFlags oldFlags = VConActive->mn_Flags);

	CVConGroup* pGr = ((CVConGroup*)VConActive->mp_Group);
	_ASSERTE(pGr);
	bMaximized = pGr ? !pGr->mb_PaneMaximized : false;

	CVConGroup* pActiveGrp = GetRootOfVCon(VConActive.VCon());

	VConFlags Set = bMaximized ? vf_Maximized : vf_None;

	// !!!   Do NOT use EnumVCon here because   !!!
	// !!! EnumVCon uses flags must be set here !!!

	CVConGuard VCon;

	// Update flags
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		if (VCon.Attach(gp_VCon[i]))
		{
			if (GetRootOfVCon(VCon.VCon()) != pActiveGrp)
				continue;

			pGr = ((CVConGroup*)VCon->mp_Group);
			_ASSERTE(pGr);
			while (pGr)
			{
				pGr->mb_PaneMaximized = bMaximized;
				pGr = pGr->mp_Parent;
			}

			VCon->SetFlags(Set, vf_Maximized, (int)i);
		}
	}

	pActiveGrp->ShowAllVCon(SW_SHOW);
	gpConEmu->OnSize();
}

void CVConGroup::OnGuiFocused(bool abFocus, bool abForceChild /*= FALSE*/)
{
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
			VCon->RCon()->OnGuiFocused(abFocus, abForceChild);
	}
}

void CVConGroup::OnConsoleResize(bool abSizingToDo)
{
	DEBUGSTRERR(L"CVConGroup::OnConsoleResize must NOT!!! be called while CONSOLE size is changed (from console)\n");

	//MSetter lInConsoleResize(&mb_InConsoleResize);
	// Выполняться должно в нити окна, иначе можем повиснуть
	_ASSERTE(isMainThread() && !gpConEmu->isIconic());

	//COORD c = ConsoleSizeFromWindow();
	RECT client = gpConEmu->ClientRect();

	// Проверим, вдруг не отработал isIconic
	if (client.bottom > 10)
	{
		CVConGuard VCon(gp_VActive);
		gpConEmu->AutoSizeFont(client, CER_MAINCLIENT);
		RECT c = CalcRect(CER_CONSOLE_CUR, client, CER_MAINCLIENT, gp_VActive);
		// чтобы не насиловать консоль лишний раз - реальное изменение ее размеров только
		// при отпускании мышкой рамки окна
		BOOL lbSizeChanged = FALSE;
		int nCurConWidth = 0;
		int nCurConHeight = 0;

		if (gp_VActive)
		{
			nCurConWidth = (int)gp_VActive->RCon()->TextWidth();
			nCurConHeight = (int)gp_VActive->RCon()->TextHeight();
			lbSizeChanged = (c.right != nCurConWidth || c.bottom != nCurConHeight);
		}

		if (gpSet->isLogging())
		{
			char szInfo[160]; wsprintfA(szInfo, "OnConsoleResize: lbSizeChanged=%i, client={{%i,%i},{%i,%i}}, CalcCon={%i,%i}, CurCon={%i,%i}",
			                            lbSizeChanged, client.left, client.top, client.right, client.bottom,
			                            c.right, c.bottom, nCurConWidth, nCurConHeight);
			CVConGroup::LogString(szInfo);
		}

		if (!gpConEmu->isSizing() &&
		        (abSizingToDo /*после реального ресайза мышкой*/ ||
		         //gpConEmu->isPostUpdateWindowSize() /*после появления/скрытия табов*/ ||
		         lbSizeChanged /*или размер в виртуальной консоли не совпадает с расчетным*/))
		{
			//gpConEmu->SetPostUpdateWindowSize(false);

			if (isNtvdm())
			{
				gpConEmu->SyncNtvdm();
			}
			else
			{
				if ((gpConEmu->WindowMode == wmNormal) && !abSizingToDo)
					SyncWindowToConsole(); // -- функция пустая, игнорируется
				else
					SyncConsoleToWindow();

				gpConEmu->OnSize(true);
			}

			//_ASSERTE(gp_VActive!=nullptr);
			if (gp_VActive)
			{
				g_LastConSize = MakeCoord(gp_VActive->GetTextWidth(), gp_VActive->GetTextHeight());
			}

			//// Запомнить "идеальный" размер окна, выбранный пользователем
			//if (abSizingToDo)
			//	gpConEmu->UpdateIdealRect();

			//if (lbSizingToDo && !mb_isFullScreen && !isZoomed() && !isIconic()) {
			//	GetWindowRect(ghWnd, &mrc_Ideal);
			//}
		}
		else if (gp_VActive
		        && (g_LastConSize.X != (int)gp_VActive->GetTextWidth()
		            || g_LastConSize.Y != (int)gp_VActive->GetTextHeight()))
		{
			// По идее, сюда мы попадаем только для 16-бит приложений
			if (isNtvdm())
				gpConEmu->SyncNtvdm();

			g_LastConSize = MakeCoord(gp_VActive->GetTextWidth(), gp_VActive->GetTextHeight());
		}
	}
}

// вызывается из CConEmuMain::OnSize
void CVConGroup::ReSizePanes(RECT workspace)
{
	//RECT mainClient = MakeRect(newClientWidth,newClientHeight);
	if (!gp_VActive)
	{
		// Еще нету никого
		_ASSERTE(gp_VActive); //-V571
		return;
	}

	CVConGuard VCon(gp_VActive);
	CVirtualConsole* pVCon = VCon.VCon();

	CVConGroup::MoveAllVCon(pVCon, workspace);
}

void CVConGroup::NotifyChildrenWindows()
{
	// Issue 878: ConEmu - Putty: Can't select in putty when ConEmu change display
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (VCon.Attach(gp_VCon[i]) && VCon->isVisible())
		{
			HWND hGuiWnd = VCon->GuiWnd(); // Child GUI Window
			if (hGuiWnd)
			{
				CRealConsole* pRCon = VCon->RCon();
				pRCon->GuiNotifyChildWindow();

				////VCon->RCon()->SetOtherWindowPos(hGuiWnd, nullptr, int X, int Y, int cx, int cy, UINT uFlags)

				//RECT rcChild = {};
				//GetWindowRect(hGuiWnd, &rcChild);
				////MapWindowPoints(nullptr, VCon->GetBack(), (LPPOINT)&rcChild, 2);

				//WPARAM wParam = 0;
				//LPARAM lParam = MAKELPARAM(rcChild.left, rcChild.top);
				////pRCon->PostConsoleMessage(hGuiWnd, WM_MOVE, wParam, lParam);

				//wParam = ::IsZoomed(hGuiWnd) ? SIZE_MAXIMIZED : SIZE_RESTORED;
				//lParam = MAKELPARAM(rcChild.right-rcChild.left, rcChild.bottom-rcChild.top);
				////pRCon->PostConsoleMessage(hGuiWnd, WM_SIZE, wParam, lParam);
			}
		}
	}
}

bool CVConGroup::isInGroup(CVirtualConsole* apVCon, CVConGroup* apGroup)
{
	if (!apVCon)
		return false;
	if (!apGroup)
		return true;

	CVConGroup* pGr = ((CVConGroup*)apVCon->mp_Group);
	while (pGr)
	{
		if (pGr == apGroup)
			return true;
		pGr = pGr->mp_Parent;
	}

	return false;
}

// rpActiveVCon <<== receives active VCon in group (it may NOT be active in ConEmu)
bool CVConGroup::isGroup(CVirtualConsole* apVCon, CVConGroup** rpRoot /*= nullptr*/, CVConGuard* rpActiveVCon /*= nullptr*/)
{
	if (rpRoot)
		*rpRoot = nullptr;

	if (!apVCon)
		return false;

	CVConGroup* pGr;

	// If needs only the state of split/non-split
	if (!rpRoot && !rpActiveVCon)
	{
		pGr = (CVConGroup*)apVCon->mp_Group;
		return (pGr->mp_Parent != nullptr);
	}

	pGr = GetRootOfVCon(apVCon);
	if (!pGr)
		return false;

	int nGroupPanes = pGr->GetGroupPanes(nullptr);
	if (nGroupPanes <= 1)
		return false;

	if (rpRoot)
		*rpRoot = pGr;

	if (rpActiveVCon)
	{
		if (isActiveGroupVCon(apVCon))
			rpActiveVCon->Attach(apVCon);
		else
			rpActiveVCon->Attach((CVirtualConsole*)pGr->mp_ActiveGroupVConPtr);
	}

	return true;
}

CVConGroup* CVConGroup::GetLeafLeft() const
{
	AssertThisRet(nullptr);
	const CVConGroup* p = this;
	while (p->mp_Grp1)
		p = p->mp_Grp1;
	return const_cast<CVConGroup*>(p);
}

CVConGroup* CVConGroup::GetLeafRight() const
{
	AssertThisRet(nullptr);
	CVConGroup* p = mp_Grp2;
	if (!p)
		return nullptr;
	return p->GetLeafLeft();
}

void CVConGroup::PopulateSplitPanes(MArray<CVConGuard*>& VCons) const
{
	CVConGroup* pFirst = GetLeafLeft();
	if (!pFirst || !pFirst->mp_Item)
	{
		_ASSERTE(pFirst && pFirst->mp_Item);
		_ASSERTE(GetLeafRight() == nullptr);
		LogString(L"PopulateSplitPanes error: left leaf is null");
		return;
	}

	auto findVCon = [&VCons](const CVirtualConsole* pVCon)
	{
		UINT nIndex = 0; // 0 - split active console, otherwise - 1-based index of the console to split
		for (ssize_t i = 0; i < VCons.size(); ++i)
		{
			if (VCons[i]->VCon() == pVCon)
			{
				nIndex = static_cast<UINT>(i) + 1;
				break;
			}
		}
		return nIndex;
	};

	// VConGroup node has left and right leafs. Splits are always done via splitting *left* node.
	// We need to know the index of *left* node to make proper split.

	// 0 - split active console, otherwise - 1-based index of the console to split
	UINT nParent = findVCon(pFirst->mp_Item);
	if (nParent == 0)
	{
		// Should be executer only for the root
		_ASSERTE(this == GetRootGroup());
		_ASSERTE(VCons.empty());

		// First pane, the anchor, is started layered!
		pFirst->mp_Item->RCon()->UpdateStartArgs(
			RConStartArgsEx::eSplitNone, DefaultSplitValue, 0, (pFirst->mp_Item == gp_VActive));

		// Remember we have processed
		CVConGuard* pFirstVCon = new CVConGuard(pFirst->mp_Item);
		VCons.push_back(pFirstVCon);
		nParent = static_cast<UINT>(VCons.size());
		_ASSERTE(nParent == findVCon(pFirst->mp_Item));
	}

	// Now update split properties of the second pane (the splitter)
	CVConGroup* pSecond = GetLeafRight();
	if (!pSecond || !pSecond->mp_Item)
	{
		// No splits?
		_ASSERTE(m_SplitType == RConStartArgsEx::eSplitNone);
		pSecond = nullptr;
	}
	else
	{
		// Update split properties
		_ASSERTE(m_SplitType != RConStartArgsEx::eSplitNone);
		_ASSERTE(nParent != 0 && nParent <= (UINT)VCons.size());
		pSecond->mp_Item->RCon()->UpdateStartArgs(
			m_SplitType, mn_SplitPercent10, nParent, (pSecond->mp_Item == gp_VActive));

		// Remember we have processed
		UINT nSecond = findVCon(pSecond->mp_Item);
		if (nSecond == 0)
		{
			CVConGuard* pSecondVCon = new CVConGuard(pSecond->mp_Item);
			VCons.push_back(pSecondVCon);
			_ASSERTE(findVCon(pSecond->mp_Item) == static_cast<UINT>(VCons.size()));
		}
		else
		{
			_ASSERTE(nSecond == 0);
			LogString(L"PopulateSplitPanes error: right leaf should not be stored yet");
		}
	}

	if (mp_Grp1)
	{
		mp_Grp1->PopulateSplitPanes(VCons);
	}
	if (mp_Grp2)
	{
		mp_Grp2->PopulateSplitPanes(VCons);
	}
}

CEStr CVConGroup::GetTasks()
{
	CEStr result;
	wchar_t* pszAll = nullptr;
	wchar_t* pszTask[MAX_CONSOLE_COUNT] = {};
	size_t nTaskLen[MAX_CONSOLE_COUNT] = {};
	size_t t = 0, nAllLen = 0, activeTask = (size_t)-1;

	_ASSERTE(isMainThread());

	// Need to correct splits storing
	MSectionLockSimple lockGroups; lockGroups.Lock(gpcs_VGroups);
	// Favor VCon tab index, but group them via splits
	MArray<CVConGroup*> groups;
	MArray<CVConGuard*> addVCon;
	for (size_t i = 0; i < countof(gp_VCon); i++)
	{
		CVConGuard VCon;
		if (!GetVCon(i, &VCon))
			break;
		CVConGroup* pRoot = GetRootOfVCon(VCon.VCon());
		if (!pRoot)
		{
			_ASSERTE(pRoot); //-V571
			continue;
		}
		// Already appended?
		ssize_t q = 0;
		while (q < groups.size())
		{
			if (groups[q] == pRoot)
				break;
			q++;
		}
		if (q == groups.size())
		{
			pRoot->PopulateSplitPanes(addVCon);
			// It's ready
			groups.push_back(pRoot);
		}
	}

	_ASSERTE(GetConCount() == addVCon.size());

	for (ssize_t i = 0; i < addVCon.size(); i++)
	{
		CVConGuard* VCon = addVCon[i];
		if (!VCon)
			continue;

		if (VCon->VCon() == gp_VActive)
			activeTask = t;

		CEStr pszCmd = VCon->VCon()->RCon()->CreateCommandLine(true);
		delete VCon;
		addVCon[i] = nullptr;

		if (pszCmd.IsEmpty())
			continue;

		const size_t nLen = nTaskLen[t] = _tcslen(pszCmd);
		pszTask[t++] = pszCmd.Detach();
		nAllLen += nLen + 6; // + "\r\n\r\n>*"
	}


	// Nothing to return?
	if (nAllLen == 0)
		return nullptr;

	// Allocate memory for result Task
	nAllLen += 3;
	pszAll = result.GetBuffer(nAllLen);
	if (!pszAll)
		return nullptr;
	wchar_t* psz = pszAll;

	// All data is prepared, concatenate the result Task
	for (size_t i = 0; i < t; i++)
	{
		_ASSERTE(pszTask[i] != nullptr);

		if (i)
		{
			_wcscpy_c(psz, 3, L"\r\n");
			psz += 2;
		}

		if (i == activeTask)
		{
			*(psz++) = L'>';
			if (pszTask[i][0] != L'*')
				*(psz++) = L' ';
		}

		_wcscpy_c(psz, nTaskLen[i]+1, pszTask[i]);
		psz += nTaskLen[i];

		_wcscpy_c(psz, 3, L"\r\n");
		psz += _tcslen(psz);

		SafeFree(pszTask[i]);
	}

	_ASSERTE((psz > pszAll) && ((size_t)(psz - pszAll) < nAllLen));
	*psz = 0; // ASCIIZ
	return result;
}
