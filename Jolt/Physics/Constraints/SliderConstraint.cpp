// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/ObjectStream/TypeDeclarations.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#ifdef JPH_DEBUG_RENDERER
	#include <Jolt/Renderer/DebugRenderer.h>
#endif // JPH_DEBUG_RENDERER

JPH_NAMESPACE_BEGIN

JPH_IMPLEMENT_SERIALIZABLE_VIRTUAL(SliderConstraintSettings)
{
	JPH_ADD_BASE_CLASS(SliderConstraintSettings, TwoBodyConstraintSettings)

	JPH_ADD_ENUM_ATTRIBUTE(SliderConstraintSettings, mSpace)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mPoint1)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mSliderAxis1)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mNormalAxis1)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mPoint2)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mSliderAxis2)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mNormalAxis2)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mLimitsMin)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mLimitsMax)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mMaxFrictionForce)
	JPH_ADD_ATTRIBUTE(SliderConstraintSettings, mMotorSettings)
}

void SliderConstraintSettings::SetPoint(const Body &inBody1, const Body &inBody2)
{
	JPH_ASSERT(mSpace == EConstraintSpace::WorldSpace);

	// Determine anchor point: If any of the bodies can never be dynamic use the other body as anchor point
	Vec3 anchor;
	if (!inBody1.CanBeKinematicOrDynamic())
		anchor = inBody2.GetCenterOfMassPosition();
	else if (!inBody2.CanBeKinematicOrDynamic())
		anchor = inBody1.GetCenterOfMassPosition();
	else
	{
		// Otherwise use weighted anchor point towards the lightest body
		float inv_m1 = inBody1.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked();
		float inv_m2 = inBody2.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked();
		anchor = (inv_m1 * inBody1.GetCenterOfMassPosition() + inv_m2 * inBody2.GetCenterOfMassPosition()) / (inv_m1 + inv_m2);
	}

	mPoint1 = mPoint2 = anchor;
}

void SliderConstraintSettings::SetSliderAxis(Vec3Arg inSliderAxis)
{
	JPH_ASSERT(mSpace == EConstraintSpace::WorldSpace);

	mSliderAxis1 = mSliderAxis2 = inSliderAxis;
	mNormalAxis1 = mNormalAxis2 = inSliderAxis.GetNormalizedPerpendicular();
}

void SliderConstraintSettings::SaveBinaryState(StreamOut &inStream) const
{ 
	ConstraintSettings::SaveBinaryState(inStream);

	inStream.Write(mSpace);
	inStream.Write(mPoint1);
	inStream.Write(mSliderAxis1);
	inStream.Write(mNormalAxis1);
	inStream.Write(mPoint2);
	inStream.Write(mSliderAxis2);
	inStream.Write(mNormalAxis2);
	inStream.Write(mLimitsMin);
	inStream.Write(mLimitsMax);
	inStream.Write(mMaxFrictionForce);
	mMotorSettings.SaveBinaryState(inStream);
}

void SliderConstraintSettings::RestoreBinaryState(StreamIn &inStream)
{
	ConstraintSettings::RestoreBinaryState(inStream);

	inStream.Read(mSpace);
	inStream.Read(mPoint1);
	inStream.Read(mSliderAxis1);
	inStream.Read(mNormalAxis1);
	inStream.Read(mPoint2);
	inStream.Read(mSliderAxis2);
	inStream.Read(mNormalAxis2);
	inStream.Read(mLimitsMin);
	inStream.Read(mLimitsMax);
	inStream.Read(mMaxFrictionForce);
	mMotorSettings.RestoreBinaryState(inStream);
}

TwoBodyConstraint *SliderConstraintSettings::Create(Body &inBody1, Body &inBody2) const
{
	return new SliderConstraint(inBody1, inBody2, *this);
}

SliderConstraint::SliderConstraint(Body &inBody1, Body &inBody2, const SliderConstraintSettings &inSettings) :
	TwoBodyConstraint(inBody1, inBody2, inSettings),
	mLocalSpacePosition1(inSettings.mPoint1),
	mLocalSpacePosition2(inSettings.mPoint2),
	mLocalSpaceSliderAxis1(inSettings.mSliderAxis1),
	mLocalSpaceNormal1(inSettings.mNormalAxis1),
	mMaxFrictionForce(inSettings.mMaxFrictionForce),
	mMotorSettings(inSettings.mMotorSettings)
{
	// Store inverse of initial rotation from body 1 to body 2 in body 1 space:
	//
	// q20 = q10 r0 
	// <=> r0 = q10^-1 q20 
	// <=> r0^-1 = q20^-1 q10
	//
	// where:
	//
	// q10, q20 = world space initial orientation of body 1 and 2
	// r0 = initial rotation rotation from body 1 to body 2 in local space of body 1
	//
	// We can also write this in terms of the constraint matrices:
	// 
	// q20 c2 = q10 c1
	// <=> q20 = q10 c1 c2^-1
	// => r0 = c1 c2^-1
	// <=> r0^-1 = c2 c1^-1
	// 
	// where:
	// 
	// c1, c2 = matrix that takes us from body 1 and 2 COM to constraint space 1 and 2
	if (inSettings.mSliderAxis1 == inSettings.mSliderAxis2 && inSettings.mNormalAxis1 == inSettings.mNormalAxis2)
	{
		// Axis are the same -> identity transform
		mInvInitialOrientation = Quat::sIdentity();
	}
	else
	{
		Mat44 constraint1(Vec4(inSettings.mSliderAxis1, 0), Vec4(inSettings.mNormalAxis1, 0), Vec4(inSettings.mSliderAxis1.Cross(inSettings.mNormalAxis1), 0), Vec4(0, 0, 0, 1));
		Mat44 constraint2(Vec4(inSettings.mSliderAxis2, 0), Vec4(inSettings.mNormalAxis2, 0), Vec4(inSettings.mSliderAxis2.Cross(inSettings.mNormalAxis2), 0), Vec4(0, 0, 0, 1));
		mInvInitialOrientation = constraint2.GetQuaternion() * constraint1.GetQuaternion().Conjugated();
	}

	if (inSettings.mSpace == EConstraintSpace::WorldSpace)
	{
		// If all properties were specified in world space, take them to local space now
		Mat44 inv_transform1 = inBody1.GetInverseCenterOfMassTransform();
		mLocalSpacePosition1 = inv_transform1 * mLocalSpacePosition1;
		mLocalSpaceSliderAxis1 = inv_transform1.Multiply3x3(mLocalSpaceSliderAxis1).Normalized();
		mLocalSpaceNormal1 = inv_transform1.Multiply3x3(mLocalSpaceNormal1).Normalized();

		mLocalSpacePosition2 = inBody2.GetInverseCenterOfMassTransform() * mLocalSpacePosition2;

		// Constraints were specified in world space, so we should have replaced c1 with q10^-1 c1 and c2 with q20^-1 c2
		// => r0^-1 = (q20^-1 c2) (q10^-1 c1)^1 = q20^-1 (c2 c1^-1) q10
		mInvInitialOrientation = inBody2.GetRotation().Conjugated() * mInvInitialOrientation * inBody1.GetRotation();
	}

	// Calculate 2nd local space normal
	mLocalSpaceNormal2 = mLocalSpaceSliderAxis1.Cross(mLocalSpaceNormal1);

	// Store limits
	JPH_ASSERT(inSettings.mLimitsMin != inSettings.mLimitsMax, "Better use a fixed constraint");
	SetLimits(inSettings.mLimitsMin, inSettings.mLimitsMax);
}

void SliderConstraint::SetLimits(float inLimitsMin, float inLimitsMax)
{
	JPH_ASSERT(inLimitsMin <= 0.0f);
	JPH_ASSERT(inLimitsMax >= 0.0f);
	mLimitsMin = inLimitsMin;
	mLimitsMax = inLimitsMax;
	mHasLimits = mLimitsMin != -FLT_MAX || mLimitsMax != FLT_MAX;
}

void SliderConstraint::CalculateR1R2U(Mat44Arg inRotation1, Mat44Arg inRotation2)
{
	// Calculate points relative to body
	mR1 = inRotation1 * mLocalSpacePosition1;
	mR2 = inRotation2 * mLocalSpacePosition2;

	// Calculate X2 + R2 - X1 - R1
	mU = mBody2->GetCenterOfMassPosition() + mR2 - mBody1->GetCenterOfMassPosition() - mR1;
}

void SliderConstraint::CalculatePositionConstraintProperties(Mat44Arg inRotation1, Mat44Arg inRotation2)
{
	// Calculate world space normals
	mN1 = inRotation1 * mLocalSpaceNormal1;
	mN2 = inRotation1 * mLocalSpaceNormal2;

	mPositionConstraintPart.CalculateConstraintProperties(*mBody1, inRotation1, mR1 + mU, *mBody2, inRotation2, mR2, mN1, mN2);
}

void SliderConstraint::CalculateSlidingAxisAndPosition(Mat44Arg inRotation1)
{
	if (mHasLimits || mMotorState != EMotorState::Off || mMaxFrictionForce > 0.0f)
	{
		// Calculate world space slider axis
		mWorldSpaceSliderAxis = inRotation1 * mLocalSpaceSliderAxis1;
		
		// Calculate slide distance along axis
		mD = mU.Dot(mWorldSpaceSliderAxis);
	}
}

void SliderConstraint::CalculatePositionLimitsConstraintProperties(float inDeltaTime)
{
	// Check if distance is within limits
	if (mHasLimits && (mD <= mLimitsMin || mD >= mLimitsMax))
		mPositionLimitsConstraintPart.CalculateConstraintProperties(inDeltaTime, *mBody1, mR1 + mU, *mBody2, mR2, mWorldSpaceSliderAxis);
	else
		mPositionLimitsConstraintPart.Deactivate();
}

void SliderConstraint::CalculateMotorConstraintProperties(float inDeltaTime)
{
	switch (mMotorState)
	{
	case EMotorState::Off:
		if (mMaxFrictionForce > 0.0f)
			mMotorConstraintPart.CalculateConstraintProperties(inDeltaTime, *mBody1, mR1 + mU, *mBody2, mR2, mWorldSpaceSliderAxis);
		else
			mMotorConstraintPart.Deactivate();
		break;

	case EMotorState::Velocity:
		mMotorConstraintPart.CalculateConstraintProperties(inDeltaTime, *mBody1, mR1 + mU, *mBody2, mR2, mWorldSpaceSliderAxis, -mTargetVelocity);
		break;

	case EMotorState::Position:
		mMotorConstraintPart.CalculateConstraintProperties(inDeltaTime, *mBody1, mR1 + mU, *mBody2, mR2, mWorldSpaceSliderAxis, 0.0f, mD - mTargetPosition, mMotorSettings.mFrequency, mMotorSettings.mDamping);
		break;
	}	
}

void SliderConstraint::SetupVelocityConstraint(float inDeltaTime)
{
	// Calculate constraint properties that are constant while bodies don't move
	Mat44 rotation1 = Mat44::sRotation(mBody1->GetRotation());
	Mat44 rotation2 = Mat44::sRotation(mBody2->GetRotation());
	CalculateR1R2U(rotation1, rotation2);
	CalculatePositionConstraintProperties(rotation1, rotation2);
	mRotationConstraintPart.CalculateConstraintProperties(*mBody1, rotation1, *mBody2, rotation2);
	CalculateSlidingAxisAndPosition(rotation1);
	CalculatePositionLimitsConstraintProperties(inDeltaTime);
	CalculateMotorConstraintProperties(inDeltaTime);
}

void SliderConstraint::WarmStartVelocityConstraint(float inWarmStartImpulseRatio)
{
	// Warm starting: Apply previous frame impulse
	mMotorConstraintPart.WarmStart(*mBody1, *mBody2, mWorldSpaceSliderAxis, inWarmStartImpulseRatio);
	mPositionConstraintPart.WarmStart(*mBody1, *mBody2, mN1, mN2, inWarmStartImpulseRatio);
	mRotationConstraintPart.WarmStart(*mBody1, *mBody2, inWarmStartImpulseRatio);
	mPositionLimitsConstraintPart.WarmStart(*mBody1, *mBody2, mWorldSpaceSliderAxis, inWarmStartImpulseRatio);
}

bool SliderConstraint::SolveVelocityConstraint(float inDeltaTime)
{
	// Solve motor
	bool motor = false;
	if (mMotorConstraintPart.IsActive())
	{
		switch (mMotorState)
		{
		case EMotorState::Off:
			{
				float max_lambda = mMaxFrictionForce * inDeltaTime;
				motor = mMotorConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, -max_lambda, max_lambda);
				break;
			}	

		case EMotorState::Velocity:
		case EMotorState::Position:
			motor = mMotorConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, inDeltaTime * mMotorSettings.mMinForceLimit, inDeltaTime * mMotorSettings.mMaxForceLimit);
			break;
		}
	}

	// Solve position constraint along 2 axis
	bool pos = mPositionConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2, mN1, mN2);

	// Solve rotation constraint
	bool rot = mRotationConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2);

	// Solve limits along slider axis
	bool limit = false;
	if (mPositionLimitsConstraintPart.IsActive())
	{
		if (mD <= mLimitsMin)
			limit = mPositionLimitsConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, 0, FLT_MAX);
		else
		{
			JPH_ASSERT(mD >= mLimitsMax);
			limit = mPositionLimitsConstraintPart.SolveVelocityConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, -FLT_MAX, 0);
		}
	}

	return motor || pos || rot || limit;
}

bool SliderConstraint::SolvePositionConstraint(float inDeltaTime, float inBaumgarte)
{
	// Motor operates on velocities only, don't call SolvePositionConstraint

	// Solve position constraint along 2 axis
	Mat44 rotation1 = Mat44::sRotation(mBody1->GetRotation());
	Mat44 rotation2 = Mat44::sRotation(mBody2->GetRotation());
	CalculateR1R2U(rotation1, rotation2);
	CalculatePositionConstraintProperties(rotation1, rotation2);
	bool pos = mPositionConstraintPart.SolvePositionConstraint(*mBody1, *mBody2, mU, mN1, mN2, inBaumgarte);

	// Solve rotation constraint
	mRotationConstraintPart.CalculateConstraintProperties(*mBody1, Mat44::sRotation(mBody1->GetRotation()), *mBody2, Mat44::sRotation(mBody2->GetRotation()));
	bool rot = mRotationConstraintPart.SolvePositionConstraint(*mBody1, *mBody2, mInvInitialOrientation, inBaumgarte);

	// Solve limits along slider axis
	bool limit = false;
	if (mHasLimits)
	{
		rotation1 = Mat44::sRotation(mBody1->GetRotation());
		rotation2 = Mat44::sRotation(mBody2->GetRotation());
		CalculateR1R2U(rotation1, rotation2);
		CalculateSlidingAxisAndPosition(rotation1);
		CalculatePositionLimitsConstraintProperties(inDeltaTime);
		if (mPositionLimitsConstraintPart.IsActive())
		{
			if (mD <= mLimitsMin)
				limit = mPositionLimitsConstraintPart.SolvePositionConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, mD - mLimitsMin, inBaumgarte);
			else
			{
				JPH_ASSERT(mD >= mLimitsMax);
				limit = mPositionLimitsConstraintPart.SolvePositionConstraint(*mBody1, *mBody2, mWorldSpaceSliderAxis, mD - mLimitsMax, inBaumgarte);
			}
		}
	}

	return pos || rot || limit;
}

#ifdef JPH_DEBUG_RENDERER
void SliderConstraint::DrawConstraint(DebugRenderer *inRenderer) const
{
	Mat44 transform1 = mBody1->GetCenterOfMassTransform();
	Mat44 transform2 = mBody2->GetCenterOfMassTransform();

	// Transform the local positions into world space
	Vec3 slider_axis = transform1.Multiply3x3(mLocalSpaceSliderAxis1);
	Vec3 position1 = transform1 * mLocalSpacePosition1;
	Vec3 position2 = transform2 * mLocalSpacePosition2;

	// Draw constraint
	inRenderer->DrawMarker(position1, Color::sRed, 0.1f);
	inRenderer->DrawMarker(position2, Color::sGreen, 0.1f);
	inRenderer->DrawLine(position1, position2, Color::sGreen);

	// Draw motor
	switch (mMotorState)
	{
	case EMotorState::Position:
		inRenderer->DrawMarker(position1 + mTargetPosition * slider_axis, Color::sYellow, 1.0f);
		break;

	case EMotorState::Velocity:
		{
			Vec3 cur_vel = (mBody2->GetLinearVelocity() - mBody1->GetLinearVelocity()).Dot(slider_axis) * slider_axis;
			inRenderer->DrawLine(position2, position2 + cur_vel, Color::sBlue);
			inRenderer->DrawArrow(position2 + cur_vel, position2 + mTargetVelocity * slider_axis, Color::sRed, 0.1f);
			break;
		}

	case EMotorState::Off:
		break;
	}
}

void SliderConstraint::DrawConstraintLimits(DebugRenderer *inRenderer) const
{
	if (mHasLimits)
	{
		Mat44 transform1 = mBody1->GetCenterOfMassTransform();
		Mat44 transform2 = mBody2->GetCenterOfMassTransform();

		// Transform the local positions into world space
		Vec3 slider_axis = transform1.Multiply3x3(mLocalSpaceSliderAxis1);
		Vec3 position1 = transform1 * mLocalSpacePosition1;
		Vec3 position2 = transform2 * mLocalSpacePosition2;

		// Calculate the limits in world space
		Vec3 limits_min = position1 + mLimitsMin * slider_axis;
		Vec3 limits_max = position1 + mLimitsMax * slider_axis;

		inRenderer->DrawLine(limits_min, position1, Color::sWhite);
		inRenderer->DrawLine(position2, limits_max, Color::sWhite);

		inRenderer->DrawMarker(limits_min, Color::sWhite, 0.1f);
		inRenderer->DrawMarker(limits_max, Color::sWhite, 0.1f);
	}
}
#endif // JPH_DEBUG_RENDERER

void SliderConstraint::SaveState(StateRecorder &inStream) const
{
	TwoBodyConstraint::SaveState(inStream);

	mMotorConstraintPart.SaveState(inStream);
	mPositionConstraintPart.SaveState(inStream);
	mRotationConstraintPart.SaveState(inStream);
	mPositionLimitsConstraintPart.SaveState(inStream);

	inStream.Write(mMotorState);
	inStream.Write(mTargetVelocity);
	inStream.Write(mTargetPosition);	
}

void SliderConstraint::RestoreState(StateRecorder &inStream)
{
	TwoBodyConstraint::RestoreState(inStream);

	mMotorConstraintPart.RestoreState(inStream);
	mPositionConstraintPart.RestoreState(inStream);
	mRotationConstraintPart.RestoreState(inStream);
	mPositionLimitsConstraintPart.RestoreState(inStream);

	inStream.Read(mMotorState);
	inStream.Read(mTargetVelocity);
	inStream.Read(mTargetPosition);
}

Mat44 SliderConstraint::GetConstraintToBody1Matrix() const
{ 
	return Mat44(Vec4(mLocalSpaceSliderAxis1, 0), Vec4(mLocalSpaceNormal1, 0), Vec4(mLocalSpaceNormal2, 0), Vec4(mLocalSpacePosition1, 1)); 
}

Mat44 SliderConstraint::GetConstraintToBody2Matrix() const 
{ 
	Mat44 mat = Mat44::sRotation(mInvInitialOrientation).Multiply3x3(Mat44(Vec4(mLocalSpaceSliderAxis1, 0), Vec4(mLocalSpaceNormal1, 0), Vec4(mLocalSpaceNormal2, 0), Vec4(0, 0, 0, 1))); 
	mat.SetTranslation(mLocalSpacePosition2); 
	return mat; 
}

JPH_NAMESPACE_END
