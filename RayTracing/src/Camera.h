#pragma once

#include "Ray.h"

#include <glm/glm.hpp>
#include <vector>

class Camera
{
public:
	Camera(float verticalFOV, float nearClip, float farClip);

	bool OnUpdate(float ts);
	void OnResize(uint32_t width, uint32_t height);

	const glm::mat4& GetProjection() const { return m_Projection; }
	const glm::mat4& GetInverseProjection() const { return m_InverseProjection; }
	const glm::mat4& GetView() const { return m_View; }
	const glm::mat4& GetInverseView() const { return m_InverseView; }
	
	const glm::vec3& GetPosition() const { return m_Position; }
	const glm::vec3& GetDirection() const { return m_ForwardDirection; }
	void SetView(const glm::vec3& position, const glm::vec3& direction);

	float GetVerticalFOV() const { return m_VerticalFOV; }
	void SetVerticalFOV(float verticalFOV);

	float GetMoveSpeed() const { return m_MoveSpeed; }
	void SetMoveSpeed(float moveSpeed) { m_MoveSpeed = moveSpeed; }

	// World-space direction of the pinhole ray through ndc (each axis -1 -> 1 across the viewport).
	// Computed per sample rather than cached per pixel so rays can be jittered for anti-aliasing.
	glm::vec3 GetRayDirection(const glm::vec2& ndc) const;

	// Thin-lens primary ray through ndc; lensSample in [0,1)^2 picks the point on the aperture.
	// GLSL counterpart: the ray generation at the top of PerPixel() in shaders/PathTrace.comp.
	Ray GenerateRay(const glm::vec2& ndc, const glm::vec2& lensSample) const;

	// Depth of field. Aperture is the lens diameter in world units (0 = pinhole, everything sharp);
	// FocusDistance is measured along the view direction.
	float GetAperture() const { return m_Aperture; }
	void SetAperture(float aperture) { m_Aperture = aperture; }
	float GetFocusDistance() const { return m_FocusDistance; }
	void SetFocusDistance(float focusDistance) { m_FocusDistance = focusDistance; }

	float GetRotationSpeed();
private:
	void RecalculateProjection();
	void RecalculateView();
private:
	glm::mat4 m_Projection{ 1.0f };
	glm::mat4 m_View{ 1.0f };
	glm::mat4 m_InverseProjection{ 1.0f };
	glm::mat4 m_InverseView{ 1.0f };

	float m_VerticalFOV = 45.0f;
	float m_NearClip = 0.1f;
	float m_FarClip = 100.0f;

	float m_MoveSpeed = 5.0f;

	float m_Aperture = 0.0f;
	float m_FocusDistance = 6.0f;

	glm::vec3 m_Position{0.0f, 0.0f, 0.0f};
	glm::vec3 m_ForwardDirection{0.0f, 0.0f, 0.0f};

	glm::vec2 m_LastMousePosition{ 0.0f, 0.0f };

	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
};
